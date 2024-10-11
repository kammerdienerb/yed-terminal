#include <memory>
#include <vector>
#include <deque>
#include <list>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <climits>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#include <utmp.h>
#endif

extern "C" {
#include <yed/plugin.h>
}



template<typename F>
class defer_finalizer {
    F f;
    bool moved;
  public:
    template<typename T>
    defer_finalizer(T && f_) : f(std::forward<T>(f_)), moved(false) { }

    defer_finalizer(const defer_finalizer &) = delete;

    defer_finalizer(defer_finalizer && other) : f(std::move(other.f)), moved(other.moved) {
        other.moved = true;
    }

    ~defer_finalizer() {
        if (!moved) f();
    }
};

struct {
    template<typename F>
    defer_finalizer<F> operator<<(F && f) {
        return defer_finalizer<F>(std::forward<F>(f));
    }
} deferrer;

#define TOKENPASTE(x, y) x ## y
#define TOKENPASTE2(x, y) TOKENPASTE(x, y)
#define defer auto TOKENPASTE2(__deferred_lambda_call, __COUNTER__) = deferrer << [&]



#define DBG_LOG_ON

#define LOG__XSTR(x) #x
#define LOG_XSTR(x) LOG__XSTR(x)

#define LOG(...)                                                   \
do {                                                               \
    LOG_FN_ENTER();                                                \
    yed_log(__VA_ARGS__);                                          \
    LOG_EXIT();                                                    \
} while (0)

#define ELOG(...)                                                  \
do {                                                               \
    LOG_FN_ENTER();                                                \
    yed_log("[!] " __VA_ARGS__);                                   \
    LOG_EXIT();                                                    \
} while (0)

#ifdef DBG_LOG_ON
#define DBG(...)                                                   \
do {                                                               \
    if (yed_var_is_truthy("terminal-debug-log")) {                 \
        LOG_FN_ENTER();                                            \
        yed_log(__FILE__ ":" LOG_XSTR(__LINE__) ": " __VA_ARGS__); \
        LOG_EXIT();                                                \
    }                                                              \
} while (0)
#else
#define DBG(...) ;
#endif

#define BUFF_WRITABLE_GUARD(_buff)             \
    (_buff)->flags &= ~(BUFF_RD_ONLY);         \
    defer { (_buff)->flags |= BUFF_RD_ONLY; };




#define DEFAULT_SHELL            "/bin/bash"
#define DEFAULT_TERMVAR          "xterm-256color"
#define DEFAULT_SCROLLBACK       10000
#define DEFAULT_MAX_BLOCK_SIZE   16384
#define DEFAULT_READ_CHUNK_SIZE  1024

const char *get_shell() {
    const char *shell;

    shell = yed_get_var("terminal-shell");

    if (shell == NULL) { shell = getenv("SHELL"); }
    if (shell == NULL) { shell = DEFAULT_SHELL;   }

    return shell;
}

const char *get_termvar() {
    const char *termvar;

    termvar = yed_get_var("terminal-termvar");

    if (termvar == NULL) { termvar = DEFAULT_TERMVAR; }

    return termvar;
}

int get_scrollback() {
    int scrollback;

    if (!yed_get_var_as_int("terminal-scrollback", &scrollback)) {
        scrollback = DEFAULT_SCROLLBACK;
    }

    return scrollback;
}

int get_max_block_size() {
    int max;

    if (!yed_get_var_as_int("terminal-max-block-size", &max)) {
        max = DEFAULT_MAX_BLOCK_SIZE;
    }

    return max;
}

int get_read_chunk_size() {
    int size;

    if (!yed_get_var_as_int("terminal-read-chunk-size", &size)) {
        size = DEFAULT_READ_CHUNK_SIZE;
    }

    return size;
}

#define N_COLORS (18)
static yed_attrs colors[N_COLORS];

#define CDEFAULT          (N_COLORS - 2)
#define CDEFAULT_INACTIVE (N_COLORS - 1)

#define MODE_RESET ('!')
#define MODE_PRIV  ('?')
#define MODE_XTERM ('>')

struct CSI {
    std::vector<long> args;
    int               command  = 0;
    int               len      = 0;
    int               complete = 0;
    int               mode     = 0;

    CSI(const char *str) {
        char c;

        c = *str;

        if (!c) { return; }

        this->len = 1;

#define NEXT()            \
do {                      \
    str += 1;             \
    c = *str;             \
    if (!c) { goto out; } \
    this->len += 1;       \
} while (0)

#define IS_DELIM(_c) ((_c) == ';' || (_c) == ':' || (_c) == '?' || (_c) == ' ' || (_c) == '>' || (c) == '!' || (c) == '%')
#define IS_FINAL(_c) ((_c) >= 0x40 && (_c) <= 0x7E)

        while (IS_DELIM(c)) {
            switch (c) {
                case '!': this->mode = MODE_RESET; break;
                case '?': this->mode = MODE_PRIV;  break;
                case '>': this->mode = MODE_XTERM; break;
                case ';': this->args.push_back(0); break;
            }
            NEXT();
        }

        while (!IS_FINAL(c) && c != '\e') {
            std::string sarg;
            long        arg;

            if (!is_digit(c) && !IS_DELIM(c)) { goto out; }

            while (is_digit(c)) {
                sarg += c;
                NEXT();
            }

            sscanf(sarg.c_str(), "%ld", &arg);
            this->args.push_back(arg);

            while (IS_DELIM(c)) { NEXT(); }
        }

        if (c == '\e') {
            this->len -= 1;
            goto out;
        }

        this->command  = c;
        this->complete = 1;
out:;

#undef IS_DELIM
#undef NEXT
    }
};

struct OSC {
    long        command  = 0;
    int         len      = 0;
    std::string arg;
    int         complete = 0;

    OSC(const char *str) {
        char c;

        c = *str;

        if (!c) { return; }

        this->len = 1;

#define NEXT()            \
do {                      \
    str += 1;             \
    c = *str;             \
    if (!c) { goto out; } \
    this->len += 1;       \
} while (0)

        std::string scmd;
        while (is_digit(c)) {
            scmd += c;
            NEXT();
        }

        sscanf(scmd.c_str(), "%ld", &this->command);

        if (c != ';' && c != CTRL_G) { goto out; }

        while (c && c != CTRL_G && c != '\e') {
            if (c == '\e') {
                NEXT();
                if (c == '\\') { break; }
            }
            this->arg += c;
            NEXT();
        }

        if (c == '\e') {
            NEXT();
            if (c == '\\') {
                this->complete = 1;
            } else {
                this->len -= 1;
            }
        } else {
            this->complete = 1;
        }
out:;

#undef NEXT
    }
};

struct DCS {
    int         len      = 0;
    int         complete = 0;
    std::string str;

    DCS(const char *str) {
        char c;

        c = *str;

        if (!c) { return; }

        this->len = 1;

#define NEXT()            \
do {                      \
    str += 1;             \
    c = *str;             \
    if (!c) { goto out; } \
    this->len += 1;       \
} while (0)

        while (c) {
            if (c == '\e') {
                NEXT();
                if (c == '\\') {
                    this->complete = 1;
                    break;
                }
                this->len -= 1;
                break;;
            }
            this->str += c;
            NEXT();
        }
out:;

#undef NEXT
    }
};

struct Cell {
    yed_glyph glyph;
    yed_attrs attrs;
};

struct Line : std::vector<Cell> {
    int dirty = 0;

    void clear_cells(int width, yed_attrs attrs) {
        int prev_width = this->size();

        for (int i = 0; i < prev_width; i += 1) {
            (*this)[i] = { yed_glyph_copy(GLYPH("")), attrs };
        }

        this->resize(width, { yed_glyph_copy(GLYPH("")), attrs });
    }

    Line * new_by_donation(int width, yed_attrs attrs) {
        Line *l = new Line(0, ZERO_ATTR);

        *l = std::move(*this);
        l->clear_cells(width, attrs);

        return l;
    }

    Line(int width, yed_attrs attrs) {
        this->clear_cells(width, attrs);
    }
};

#define DEFAULT_WIDTH           (80)
#define DEFAULT_HEIGHT          (24)

struct Screen {
    std::deque<Line*>  lines;
    int                width           = 0;
    int                height          = 0;
    int                cursor_row      = 1;
    int                cursor_col      = 1;
    int                cursor_row_save = 1;
    int                cursor_col_save = 1;
    yed_attrs          attrs_save      = ZERO_ATTR;
    int                cursor_saved    = 0;
    int                scroll_t        = 0;
    int                scroll_b        = 0;
    int                scrollback      = get_scrollback();
    yed_attrs         &attrs;

    Screen(yed_attrs &_attrs) : attrs(_attrs) { }

    ~Screen() {
        for (auto linep : this->lines) { delete linep; }
    }

    Line& operator[](std::size_t idx)             { return *(lines[idx]); }
    const Line& operator[](std::size_t idx) const { return *(lines[idx]); }

    void set_cursor(int row, int col) {
        this->cursor_row = row; LIMIT(this->cursor_row, 1, this->height);
        this->cursor_col = col; LIMIT(this->cursor_col, 1, this->width);
    }

    void move_cursor(int rows, int cols) {
        this->set_cursor(this->cursor_row + rows, this->cursor_col + cols);
    }

    void save_cursor() {
        this->cursor_row_save = this->cursor_row;
        this->cursor_col_save = this->cursor_col;
        this->attrs_save      = this->attrs;
        this->cursor_saved    = 1;
    }

    void restore_cursor() {
        this->cursor_row   = this->cursor_row_save;
        this->cursor_col   = this->cursor_col_save;
        this->attrs        = this->attrs_save;
        this->cursor_saved = 0;
    }

    void make_dirty() {
        for (auto linep : this->lines) { linep->dirty = 1; }
    }

    void _delete_line(int which) {
        delete this->lines[which];
        this->lines.erase(this->lines.begin() + which);
    }

    void set_scroll(int top, int bottom) {
        this->scroll_t = top;    LIMIT(this->scroll_t, 0, this->height);
        this->scroll_b = bottom; LIMIT(this->scroll_b, 0, this->height);
    }

    int sctop()    { return this->scroll_t ? this->scroll_t : 1;            }
    int scbottom() { return this->scroll_b ? this->scroll_b : this->height; }

    void set(int row, int col, yed_glyph *g) {
        if (row > this->height || col > this->width) { return; }

        auto &line = (*this)[this->scrollback + row - 1];
        auto &cell = line[col - 1];

        cell.glyph = yed_glyph_copy(g);

        int width = yed_get_glyph_width(g);
        for (int i = 0; i < width; i += 1) {
            if (col + i > this->width) { break; }
            auto &cell = line[col - 1 + i];
            cell.attrs = this->attrs;
        }

        line.dirty = 1;
    }

    void set_current_cell(yed_glyph *g) {
        this->set(this->cursor_row, this->cursor_col, g);
    }

    void insert(int row, int col, yed_glyph *g) {
        Cell new_cell;

        new_cell.glyph = yed_glyph_copy(g);
        new_cell.attrs = this->attrs;

        auto &line = (*this)[this->scrollback + row - 1];
        line.insert(line.begin() + col - 1, new_cell);
        line.pop_back();
    }

    void del_cell(int row, int col) {
        auto &line = (*this)[this->scrollback + row - 1];
        line.erase(line.begin() + col - 1);
        line.push_back({ yed_glyph_copy(GLYPH("")), this->attrs });
        line.dirty = 1;
    }

    void clear_row_abs(int row) {
        auto &line = (*this)[row - 1];
        line.clear_cells(this->width, this->attrs);
        line.dirty = 1;
    }

    void clear_row(int row) {
        this->clear_row_abs(this->scrollback + row);
    }

    void scroll_up(yed_buffer *buffer) {
        int del_row = this->scroll_t ? this->scrollback + this->scroll_t : 1;
        int new_row = this->scrollback + this->scbottom();

        auto new_line = this->lines[del_row - 1]->new_by_donation(this->width, this->attrs);
        this->_delete_line(del_row - 1);

        if (new_row > this->lines.size()) {
            this->lines.push_back(new_line);
        } else {
            this->lines.insert(this->lines.begin() + new_row - 1, new_line);
        }

        { BUFF_WRITABLE_GUARD(buffer);
            yed_buff_delete_line_no_undo(buffer, del_row);
            yed_buff_insert_line_no_undo(buffer, new_row);
        }

        for (int i = this->sctop(); i <= this->scbottom(); i += 1) {
            auto &line = (*this)[this->scrollback + i - 1];
            ASSERT(line.size() >= this->width, "bad line width");
            line.dirty = 1;
        }

        ASSERT(this->lines.size() == this->scrollback + this->height, "rows mismatch");
    }

    void scroll_down(yed_buffer *buffer) {
        int del_row = this->scrollback + this->scbottom();
        int new_row = this->scrollback + (this->scroll_t ? this->scroll_t : 1);

        this->_delete_line(del_row - 1);
        this->lines.insert(this->lines.begin() + new_row - 1, new Line(this->width, this->attrs));

        { BUFF_WRITABLE_GUARD(buffer);
            yed_buff_delete_line_no_undo(buffer, del_row);
            yed_buff_insert_line_no_undo(buffer, new_row);
        }

        for (int i = this->sctop(); i <= this->scbottom(); i += 1) {
            auto &line = (*this)[this->scrollback + i - 1];
            ASSERT(line.size() >= this->width, "bad line width");
            line.dirty = 1;
        }

        ASSERT(this->lines.size() == this->scrollback + this->height, "rows mismatch");
    }

    void insert_line(int row, yed_buffer *buffer) {
        int del_row = this->scrollback + this->scbottom();
        int new_row = this->scrollback + row;

        this->_delete_line(del_row - 1);
        if (new_row > this->lines.size()) {
            this->lines.push_back(new Line(this->width, this->attrs));
        } else {
            this->lines.insert(this->lines.begin() + new_row - 1, new Line(this->width, this->attrs));
        }

        { BUFF_WRITABLE_GUARD(buffer);
            yed_buff_delete_line_no_undo(buffer, del_row);
            yed_buff_insert_line_no_undo(buffer, new_row);
        }

        for (int i = this->sctop(); i <= this->scbottom(); i += 1) {
            auto &line = (*this)[this->scrollback + i - 1];
            ASSERT(line.size() >= this->width, "bad line width");
            line.dirty = 1;
        }

        ASSERT(this->lines.size() == this->scrollback + this->height, "rows mismatch");
    }

    void delete_line(int row, yed_buffer *buffer) {
        int del_row = this->scrollback + row;
        int new_row = this->scrollback + this->scbottom();

        this->_delete_line(del_row - 1);
        this->lines.insert(this->lines.begin() + new_row - 1, new Line(this->width, this->attrs));

        { BUFF_WRITABLE_GUARD(buffer);
            yed_buff_delete_line_no_undo(buffer, del_row);
            yed_buff_insert_line_no_undo(buffer, new_row);
        }

        if (this->scroll_t || this->scroll_b) {
            for (int i = this->sctop(); i <= this->scbottom(); i += 1) {
                auto &line = (*this)[this->scrollback + i - 1];
                ASSERT(line.size() >= this->width, "bad line width");
                line.dirty = 1;
            }
        }

        ASSERT(this->lines.size() == this->scrollback + this->height, "rows mismatch");
    }

    void set_dimensions(int width, int height, yed_buffer *buffer) {
        int max_width;
        int num_lines;

        max_width = width;
        for (auto linep : this->lines) {
            if (linep->size() > max_width) {
                max_width = linep->size();
            }
        }

        num_lines = height + this->scrollback;

        if (num_lines > this->lines.size()) {
            while (this->lines.size() < num_lines) {
                this->lines.push_back(new Line(max_width, this->attrs));
            }
        } else {
            while (this->lines.size() > num_lines) {
                auto linep = this->lines.back();
                auto &line = *linep;

                if (line[0].glyph.c == 0) {
                    this->_delete_line(this->lines.size() - 1);
                } else {
                    this->_delete_line(0);
                }
            }
        }

        if (max_width > this->width) {
            for (auto linep : this->lines) {
                auto &line = *linep;

                line.resize(max_width, { yed_glyph_copy(GLYPH("")), this->attrs });
            }
        }

        this->width  = width;
        this->height = height;

        LIMIT(this->scroll_t, 0, this->height);
        LIMIT(this->scroll_b, 0, this->height);

        LIMIT(this->cursor_row, 1, this->height);
        LIMIT(this->cursor_col, 1, this->width);

        for (int row = MAX(1, this->scrollback - this->height); row < this->scrollback + this->height; row += 1) {
            auto &line = *(this->lines[row - 1]);
            line.dirty = 1;
        }
    }

    void write_to_buffer(yed_buffer *buffer) {
        BUFF_WRITABLE_GUARD(buffer);

        yed_line new_line = yed_new_line_with_cap(this->width);

        int row = 1;
        auto n_lines = this->lines.size();
        for (int i = 0; i < n_lines; i += 1) {
            auto &line = (*this)[i];
            if (line.dirty) {
                int n = line.size();

                yed_clear_line(&new_line);

                for (; n >= 1; n -= 1) {
                    if (line[n - 1].glyph.c != 0
                    ||  line[n - 1].attrs.flags != 0) { break; }
                }

                for (int i = 0; i < n; i += 1) {
                    auto g = line[i].glyph.c ? line[i].glyph : yed_glyph_copy(GLYPH(" "));
                    yed_line_append_glyph(&new_line, &g);
                }

                yed_buff_set_line_no_undo(buffer, row, &new_line);

                line.dirty = 0;
            }
            row += 1;
        }

        yed_free_line(&new_line);
    }
};


struct Term {
    int                valid           = 0;
    int                master_fd       = 0;
    int                slave_fd        = 0;
    int                sig_fds[2]      = { 0, 0 };
    pid_t              shell_pid       = 0;
    int                process_exited  = 0;
    int                bad_shell       = 0;
    std::thread        thr;
    std::mutex         buff_lock;
    std::vector<char>  data_buff;
    int                max_block_size  = 0;
    int                read_chunk_size = 0;
    int                delay_update    = 0;
    int                update_waiting  = 0;
    yed_buffer        *buffer          = NULL;
    yed_attrs          current_attrs   = ZERO_ATTR;
    Screen             main_screen;
    Screen             alt_screen;
    Screen            *_screen         = NULL;
    int                app_keys        = 0;
    int                auto_wrap       = 1;
    int                wrap_next       = 0;
    std::string        title;
    int                term_mode       = 1;

    static void read_thread(Term *term) {
        struct pollfd pfds[2];
        ssize_t       n;

        pfds[0].fd      = term->master_fd;
        pfds[0].events  = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd      = term->sig_fds[0];
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;

        for (;;) {
            if (poll(pfds, 2, -1) <= 0) {
                if (errno) {
                    if (errno != EINTR) {
                        term->process_exited = 1;
                    }

                    errno = 0;

                    if (term->process_exited) {
                        return;
                    }
                }
                continue;
            }

            /* The main thread has signaled us to stop. */
            if (pfds[1].revents & POLLIN) { return; }

            int max          = term->max_block_size;
            int force_update = 0;

            { std::lock_guard<std::mutex> lock(term->buff_lock);
                auto s  = term->data_buff.size();

                /* If the main thread has emptied the buffer, we need
                 * to force a new update for this new data. */
                force_update = s == 0;

                term->data_buff.resize(s + term->read_chunk_size);
                char *p = &(term->data_buff[term->data_buff.size() - term->read_chunk_size]);

                while ((n = read(term->master_fd, p, term->read_chunk_size)) > 0) {
                    term->data_buff.resize(((p + n) - term->data_buff.data()) + term->read_chunk_size);
                    p = &(term->data_buff[term->data_buff.size() - term->read_chunk_size]);

                    if (term->data_buff.size() > max) {
                        force_update = 1;
                        break;
                    }
                }

                term->data_buff.resize((p - term->data_buff.data()));
            }

            if (n <= 0) {
                if (errno == EWOULDBLOCK) {
                    errno = 0;
                } else {
                    errno                = 0;
                    term->process_exited = 1;
                    return;
                }
            }

            if (force_update && !term->update_waiting) { yed_force_update(); }
        }
    }

    Screen& screen() { return *this->_screen; }

    void resize(int width, int height) {
        struct winsize ws;

        if (this->screen().width == width && this->screen().height == height) { return; }

        ws.ws_row    = height;
        ws.ws_col    = width;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;

        if (ioctl(this->slave_fd, TIOCSWINSZ, &ws) == -1) {
            ELOG("ioctl(TIOCSWINSZ) failed with errno = %d", errno);
            return;
        }

        { BUFF_WRITABLE_GUARD(this->buffer);
            int n_rows = this->screen().scrollback + height;

            if (yed_buff_n_lines(this->buffer) < n_rows) {
                while (yed_buff_n_lines(this->buffer) < n_rows) {
                    yed_buffer_add_line_no_undo(this->buffer);
                }
            } else while (yed_buff_n_lines(this->buffer) > n_rows) {
                auto linep = this->screen().lines.back();
                auto &line = *linep;

                if (line[0].glyph.c == 0) {
                    yed_buff_delete_line_no_undo(this->buffer, yed_buff_n_lines(this->buffer));
                } else {
                    yed_buff_delete_line_no_undo(this->buffer, 1);
                }
            }
        }

        this->main_screen.set_dimensions(width, height, this->buffer);
        this->alt_screen.set_dimensions(width, height, this->buffer);

        ASSERT(yed_buff_n_lines(this->buffer) == this->screen().scrollback + height,
               "buff wrong size");

        DBG("new size %dx%d", width, height);

        this->delay_update = 1;
    }


    Term(u32 num) : main_screen(this->current_attrs),
                    alt_screen(this->current_attrs),
                    _screen(&this->main_screen) {

        char           name[64];
        pid_t          p;
        struct winsize ws;

        snprintf(name, sizeof(name), "*term%u", num);
        this->buffer = yed_get_or_create_special_rdonly_buffer(name);

        ws.ws_row    = DEFAULT_HEIGHT;
        ws.ws_col    = DEFAULT_WIDTH;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;

        if (pipe(this->sig_fds) != 0) {
            ELOG("pipe() failed with errno = %d", errno);
            errno = 0;
            return;
        }

        if (openpty(&this->master_fd, &this->slave_fd, NULL, NULL, &ws) != 0) {
            ELOG("openpty() failed with errno = %d", errno);
            errno = 0;
            return;
        }

        int print_welcome = yed_var_is_truthy("terminal-show-welcome");

        p = fork();
        if (p == 0) {
            close(this->master_fd);
            login_tty(this->slave_fd);

            setenv("TERM", get_termvar(), 1);

            if (print_welcome) {
            printf(
"Welcome to\n"
TERM_CYAN
"                _   _                      _             _ \n"
" _   _  ___  __| | | |_ ___ _ __ _ __ ___ (_)_ __   __ _| |\n"
"| | | |/ _ \\/ _` | | __/ _ \\ '__| '_ ` _ \\| | '_ \\ / _` | |\n"
"| |_| |  __/ (_| | | ||  __/ |  | | | | | | | | | | (_| | |\n"
" \\__, |\\___|\\__,_|  \\__\\___|_|  |_| |_| |_|_|_| |_|\\__,_|_|\n"
" |___/  \n\n" TERM_RESET);
            }

            char * const args[] = { (char*)get_shell(), NULL };
            execvp(get_shell(), args);
            exit(123);
        } else {
            int flags = fcntl(this->master_fd, F_GETFL);
            int err = fcntl(this->master_fd, F_SETFL, flags | O_NONBLOCK);
            (void)err;

            this->shell_pid = p;

            this->resize(DEFAULT_WIDTH, DEFAULT_HEIGHT);
            this->set_cursor(1, 1);

            this->max_block_size  = get_max_block_size();
            this->read_chunk_size = get_read_chunk_size();

            thr = std::thread(Term::read_thread, this);

            this->valid = 1;
        }
    }

    ~Term() {
        char z = 0;

        write(this->sig_fds[1], &z, 1);

        close(this->master_fd);
        close(this->slave_fd);

        thr.join();

        close(this->sig_fds[1]);
        close(this->sig_fds[0]);

        yed_free_buffer(this->buffer);
    }

    void move_cursor(int rows, int cols, int cancel_wrap = 1) {
        this->screen().move_cursor(rows, cols);
        if (cancel_wrap) { this->wrap_next = 0; }
    }

    void set_cursor(int row, int col) {
        this->screen().set_cursor(row, col);
        this->wrap_next = 0;
    }

    void save_cursor()                                        { this->screen().save_cursor();           }
    void restore_cursor()                                     { this->screen().restore_cursor();        }
    int  cursor_saved()                                       { return this->screen().cursor_saved;     }
    void set_scroll(int top, int bottom)                      { this->screen().set_scroll(top, bottom); }

    int row()               { return this->screen().cursor_row;                             }
    int scrollback_row()    { return this->screen().scrollback + this->screen().cursor_row; }
    int col()               { return this->screen().cursor_col;                             }
    int height()            { return this->screen().height;                                 }
    int width()             { return this->screen().width;                                  }
    int sctop()             { return this->screen().sctop();                                }
    int scbottom()          { return this->screen().scbottom();                             }

    void clear_row(int row) { this->screen().clear_row(row); }
    void clear_page() {
        for (int row = 1; row <= this->height(); row += 1) {
            this->clear_row(row);
        }
    }
    void reset() {
        this->current_attrs = ZERO_ATTR;
        this->app_keys      = 0;
        this->auto_wrap     = 1;
        this->wrap_next     = 0;

        this->set_scroll(0, 0);
        this->set_cursor(1, 1);
        this->clear_page();
    }

    void insert_cell(int row, int col, yed_glyph *g) {
        this->screen().insert(row, col, g);
    }

    void set_cell(int row, int col, yed_glyph *g) {
        this->screen().set(row, col, g);
    }

    void set_current_cell(yed_glyph *g) {
        this->screen().set_current_cell(g);
    }

    void delete_cell(int row, int col) {
        this->screen().del_cell(row, col);
    }

    void delete_current_cell() {
        this->delete_cell(this->row(), this->col());
    }

    void scroll_up()          { this->screen().scroll_up(this->buffer);        }
    void scroll_down()        { this->screen().scroll_down(this->buffer);      }
    void insert_line(int row) { this->screen().insert_line(row, this->buffer); }
    void delete_line(int row) { this->screen().delete_line(row, this->buffer); }

    void set_cursor_in_frame(yed_frame *frame) {
        yed_set_cursor_within_frame(frame, this->scrollback_row() + this->height() - (this->height() <= 1), this->col());
        yed_set_cursor_within_frame(frame, this->scrollback_row(), this->col());
    }

    void write_to_buffer() { this->screen().write_to_buffer(this->buffer); }

    void execute_CSI(CSI &csi) {
        long val;

#define ENC(_cmd, _mode) ((_cmd) | ((_mode) << 24))
#define RESET(_cmd)      ENC((_cmd), MODE_RESET)
#define PRIV(_cmd)       ENC((_cmd), MODE_PRIV)
#define XTERM(_cmd)      ENC((_cmd), MODE_XTERM)

#define SHIFT()                           \
do {                                      \
    if (csi.args.size()) {                \
        csi.args.erase(csi.args.begin()); \
    }                                     \
} while (0)

        /* Resources:
         *     https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
         *     https://vt100.net/docs
         *     http://www.xfree86.org/4.5.0/ctlseqs.html
         */

        switch (ENC(csi.command, csi.mode)) {
            case '@': {
                val = csi.args.size() ? csi.args[0] : 1;

                auto save = this->current_attrs;
                this->current_attrs = ZERO_ATTR;
                for (int i = 0; i < val; i += 1) {
                    this->insert_cell(this->row(), this->col(), GLYPH(" "));
                }
                this->current_attrs = save;
                break;
            }
            case 'A':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(-val, 0);
                break;
            case 'B':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(val, 0);
                break;
            case 'c': {
                const char *id = "\e[?6c";
                write(this->master_fd, id, strlen(id));
                break;
            }
            case XTERM('c'): { /* Send device attributes. */
                const char *id = "\e[>0;0;0c";
                write(this->master_fd, id, strlen(id));
                break;
            }
            case 'C':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(0, val);
                break;
            case 'D':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(0, -val);
                break;
            case 'E':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(val, 0);
                this->set_cursor(this->row(), 1);
                break;
            case 'F':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(-val, 0);
                this->set_cursor(this->row(), 1);
                break;
            case 'd':
                val = csi.args.size() ? csi.args[0] : 1;
                this->set_cursor(val, this->col());
                break;
            case 'f':
            case 'H':
                switch (csi.args.size()) {
                    case 0:
                        this->set_cursor(1, 1);
                        break;
                    case 1:
                        this->set_cursor(csi.args[0], 1);
                        break;
                    case 2:
                        this->set_cursor(csi.args[0], csi.args[1]);
                        break;
                }
                break;
            case 'G':
                val = csi.args.size() ? csi.args[0] : 1;
                this->set_cursor(this->row(), val);
                break;
            case 'J':
                if (csi.args.size() == 0) { goto J_missing; }
                switch (csi.args[0]) {
                    case 0:
                    J_missing:;
                        for (int row = this->height(); row > this->row(); row -= 1) {
                            this->clear_row(row);
                        }
                        for (int col = this->width(); col >= this->col(); col -= 1) {
                            this->set_cell(this->row(), col, GLYPH(""));
                        }
                        break;
                    case 1:
                        for (int row = 1; row < this->row(); row += 1) {
                            this->clear_row(row);
                        }
                        for (int col = 1; col >= this->col(); col += 1) {
                            this->set_cell(this->row(), col, GLYPH(""));
                        }
                        break;
                    case 2:
                        this->clear_page();
                        break;
                    case 3:
                        goto unhandled;
                        break;
                    default:
                        goto unhandled;
                        break;
                }
                break;
            case 'K':
                if (csi.args.size() == 0) { goto K_missing; }
                switch (csi.args[0]) {
                    case 0:
                    K_missing:;
                        for (int col = this->width(); col >= this->col(); col -= 1) {
                            this->set_cell(this->row(), col, GLYPH(""));
                        }
                        break;
                    case 1:
                        for (int col = 1; col <= this->col(); col += 1) {
                            this->set_cell(this->row(), col, GLYPH(""));
                        }
                        break;
                    case 2:
                        this->clear_row(this->row());
                        break;
                    default:
                        goto unhandled;
                        break;
                }
                break;
            case 'L':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->insert_line(this->row());
                }
                break;
            case 'M':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->delete_line(this->row());
                }
                break;
            case 'P':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->delete_current_cell();
                }
                break;
            case 'l':
                val = csi.args.size() ? csi.args[0] : 0;
                switch (val) {
                    default:
                        goto unhandled;
                        break;
                }
                break;
            case 'h':
                val = csi.args.size() ? csi.args[0] : 0;
                switch (val) {
                    default:
                        goto unhandled;
                        break;
                }
                break;
            case PRIV('h'):
                /* DEC Private modes enable. */
                val = csi.args.size() ? csi.args[0] : 1;
                switch (val) {
                    case 1:
                        this->app_keys = 1;
                        break;
                    case 3:
                        this->reset();
                        break;
                    case 7:
                        this->auto_wrap = 1;
                        break;
                    case 12:
                        /* Ignore blinking cursor. */
                        break;
                    case 25:
                        /* Ignore cursor show/hide. */
                        break;
                    case 1049:
                        this->_screen = &this->alt_screen;
                        this->set_cursor(1, 1);
                        this->clear_page();
                        this->screen().make_dirty();
                        DBG("alt_screen ON");
                        break;
                    default:;
                        goto unhandled;
                        break;
                }
                break;
            case PRIV('l'):
                /* DEC Private modes disable. */
                val = csi.args.size() ? csi.args[0] : 1;
                switch (val) {
                    case 1:
                        this->app_keys = 0;
                        break;
                    case 3:
                        this->reset();
                        break;
                    case 7:
                        this->auto_wrap = 0;
                        break;
                    case 12:
                        /* Ignore blinking cursor. */
                        break;
                    case 25:
                        /* Ignore cursor show/hide. */
                        break;
                    case 1049:
                        this->_screen = &this->main_screen;
                        this->screen().make_dirty();
                        DBG("alt_screen OFF");
                        break;
                    default:;
                        goto unhandled;
                        break;
                }
                break;
            case 'm':
                if (csi.args.size() == 0) { csi.args.push_back(0); }

                while (csi.args.size()) {
                    auto cmd = csi.args[0];
                    SHIFT();

                    switch (cmd) {
                        case 0:
                            this->current_attrs = ZERO_ATTR;
                            break;
                        case 1:
                            this->current_attrs.flags |= ATTR_BOLD;
                            break;
                        case 2:
                            /* Dim mode ignored. */
                            break;
                        case 3:
                            /* Italic mode ignored. */
                            break;
                        case 4:
                            this->current_attrs.flags |= ATTR_UNDERLINE;
                            break;
                        case 5:
                            /* Blinking mode ignored. */
                            break;
                        case 6:
                            /* Rapid blinking mode ignored. */
                            break;
                        case 7:
                            this->current_attrs.flags |= ATTR_INVERSE;
                            break;
                        case 8:
                            /* Hidden mode ignored. */
                            break;
                        case 9:
                            /* Strikethrough mode ignored. */
                            break;
                        case 22:
                            this->current_attrs.flags &= ~ATTR_BOLD;
                            break;
                        case 23:
                            /* Italic mode ignored. */
                            break;
                        case 24:
                            this->current_attrs.flags &= ~ATTR_UNDERLINE;
                            break;
                        case 25:
                            /* Blinking mode ignored. */
                            break;
                        case 26:
                            /* Rapid blinking mode ignored. */
                            break;
                        case 27:
                            this->current_attrs.flags &= ~ATTR_INVERSE;
                            break;
                        case 28:
                            /* Hidden mode ignored. */
                            break;
                        case 29:
                            /* Strikethrough mode ignored. */
                            break;
                        case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
                            this->current_attrs.flags &= ~ATTR_16_LIGHT_FG;
                            ATTR_SET_FG_KIND(this->current_attrs.flags, ATTR_KIND_16);
                            this->current_attrs.fg = cmd;
                            break;
                        case 38: {
                            auto which = csi.args[0];
                            SHIFT();
                            switch (which) {
                                case 2: {
                                    auto r = csi.args[0]; SHIFT();
                                    auto g = csi.args[0]; SHIFT();
                                    auto b = csi.args[0]; SHIFT();
                                    ATTR_SET_FG_KIND(this->current_attrs.flags, ATTR_KIND_RGB);
                                    this->current_attrs.fg     = RGB_32(r, g, b);
                                    this->current_attrs.flags &= ~(ATTR_16_LIGHT_FG | ATTR_16_LIGHT_BG);
                                    break;
                                }
                                case 5:
                                    ATTR_SET_FG_KIND(this->current_attrs.flags, ATTR_KIND_256);
                                    this->current_attrs.fg     = csi.args[0];
                                    this->current_attrs.flags &= ~(ATTR_16_LIGHT_FG | ATTR_16_LIGHT_BG);
                                    SHIFT();
                                    break;
                            }
                            break;
                        }
                        case 39:
                            ATTR_SET_FG_KIND(this->current_attrs.flags, ATTR_KIND_NONE);
                            this->current_attrs.fg = 0;
                            break;
                        case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                            this->current_attrs.flags &= ~ATTR_16_LIGHT_BG;
                            ATTR_SET_BG_KIND(this->current_attrs.flags, ATTR_KIND_16);
                            this->current_attrs.bg     = cmd - 10;
                            break;
                        case 48: {
                            auto which = csi.args[0];
                            SHIFT();
                            switch (which) {
                                case 2: {
                                    auto r = csi.args[0]; SHIFT();
                                    auto g = csi.args[0]; SHIFT();
                                    auto b = csi.args[0]; SHIFT();
                                    ATTR_SET_BG_KIND(this->current_attrs.flags, ATTR_KIND_RGB);
                                    this->current_attrs.bg     = RGB_32(r, g, b);
                                    this->current_attrs.flags &= ~(ATTR_16_LIGHT_FG | ATTR_16_LIGHT_BG);
                                    break;
                                }
                                case 5:
                                    ATTR_SET_BG_KIND(this->current_attrs.flags, ATTR_KIND_256);
                                    this->current_attrs.bg     = csi.args[0];
                                    this->current_attrs.flags &= ~(ATTR_16_LIGHT_FG | ATTR_16_LIGHT_BG);
                                    SHIFT();
                                    break;
                            }
                            break;
                        }
                        case 49:
                            ATTR_SET_BG_KIND(this->current_attrs.flags, ATTR_KIND_NONE);
                            this->current_attrs.bg = 0;
                            break;
                        case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                            ATTR_SET_FG_KIND(this->current_attrs.flags, ATTR_KIND_16);
                            this->current_attrs.flags |= ATTR_16_LIGHT_FG;
                            this->current_attrs.fg     = cmd - 60;
                            break;
                        case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                            ATTR_SET_BG_KIND(this->current_attrs.flags, ATTR_KIND_16);
                            this->current_attrs.flags |= ATTR_16_LIGHT_BG;
                            this->current_attrs.bg     = cmd - 70;
                            break;
                        default:
                            goto unhandled;
                            break;
                    }
                }
                break;
            case XTERM('m'):
                /* Ignore xterm key modifier options. */
                break;
            case 'n':
                val = csi.args.size() ? csi.args[0] : 0;
                switch (val) {
                    case '5': /* Report status OK. */
                        write(this->master_fd, "\e[0n", 4);
                        break;
                    case '6': { /* Report cursor location. */
                        auto response =    "\e["
                                         + std::to_string(this->row())
                                         + ";"
                                         + std::to_string(this->col())
                                         + "R";
                        write(this->master_fd, response.c_str(), response.size());
                        break;
                    }
                }
                break;
            case RESET('p'):
                this->current_attrs = ZERO_ATTR;
                this->set_cursor(1, 1);
                this->set_scroll(0, 0);
                this->clear_page();
                this->app_keys = 0;
                this->_screen = &this->main_screen;
                this->main_screen.make_dirty();
                break;
            case 'r':
                /* Set scrolling region. */
                this->set_cursor(1, 1);

                switch (csi.args.size()) {
                    case 0:
                        this->set_scroll(0, 0);
                        break;
                    case 1:
                        this->set_scroll(csi.args[0], 0);
                        break;
                    case 2:
                        if (csi.args[1] >= csi.args[0]) {
                            this->set_scroll(csi.args[0], csi.args[1]);
                        }
                        break;
                }
                break;
            case 'S':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->scroll_up();
                }
                break;
            case 'T':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->scroll_down();
                }
                break;
            case 't':
                SHIFT();
                switch (csi.args.size()) {
                    case 2:
                        /* Ignore bell volume. */
                        break;
                    case 3:
                        /* Ignore xterm window manipulations. */
                        break;
                }
                break;
            case XTERM('t'):
                /* Ignore xterm title mode controls. */
                break;
            case 'X':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->set_cell(this->row(), this->col() + i, GLYPH(""));
                }
                break;
            default:
                DBG("  UNRECOGNIZED CSI");
            unhandled:;
                DBG("  UNHANDLED CSI %c", csi.command);
/*                 LOG("Unhandled CSI %c", csi.command); */
/*                 for (auto a : csi.args) { LOG("  %ld", a); } */
        }
    }

    void execute_OSC(OSC &osc) {
        switch (osc.command) {
            case 0:
                this->title = osc.arg;
                break;
            case 4: case 10: case 11: {
                if (osc.arg == "?") {
                    char buff[128];
                    snprintf(buff, sizeof(buff),
                            "\e]%ld;rgb:%d%d/%d%d/%d%d\a",
                            osc.command,
                            0xff, 0xff,
                            0xff, 0xff,
                            0xff, 0xff);
                    write(this->master_fd, buff, strlen(buff));
                }
                break;
            case 52:
                printf("\e]52;c;%s\a", osc.arg.c_str() + 2);
                break;
            }
            case 104: case 110: case 111:
                /* Ignore color reset. */
                break;
            default:
                DBG("  UNRECOGNIZED OSC");
/*             unhandled:; */
                DBG("  UNHANDLED OSC %ld", osc.command);
        }
    }

    void update() {
        int                      do_log = yed_var_is_truthy("terminal-debug-log");
        std::vector<char>        buff;
        static std::string       incomplete_csi;
        static int               incomplete_esc;
        static std::vector<char> incomplete_utf8;
        int                      dectst     = 0;
        int                      setcharset = 0;

        if (this->delay_update) {
            this->delay_update = 0;
            return;
        }

        this->update_waiting = 1;
        std::lock_guard<std::mutex> lock(this->buff_lock);
        this->update_waiting = 0;

        buff = std::move(this->data_buff);
        this->data_buff.clear();

        if (incomplete_esc) {
            buff.insert(buff.begin(), '\e');
            incomplete_esc = 0;
        } else if (incomplete_csi.size()) {
            for (auto it = incomplete_csi.rbegin(); it != incomplete_csi.rend(); it++) {
                buff.insert(buff.begin(), *it);
            }
            incomplete_csi.clear();
        } else if (incomplete_utf8.size()) {
            for (auto it = incomplete_utf8.rbegin(); it != incomplete_utf8.rend(); it++) {
                buff.insert(buff.begin(), *it);
            }
            incomplete_utf8.clear();
        }

        if (buff.size() && buff.back() == '\e') {
            incomplete_esc = 1;
            buff.pop_back();
        }

        buff.push_back(0);

#define DUMP_DEBUG()                \
do {                                \
    if (do_log && debug.size()) {   \
        DBG("'%s'", debug.c_str()); \
        debug.clear();              \
    }                               \
} while (0)

        std::string debug;

        char        *s             = buff.data();
        size_t       len           = buff.size() - 1;
        yed_glyph   *git           = NULL;
        yed_glyph    last          = yed_glyph_copy(GLYPH(""));
        int          csi_countdown = 0;
        char        *p             = NULL;
        char         c             = 0;

        { BUFF_WRITABLE_GUARD(this->buffer);
            yed_glyph_traverse_n(s, len, git) {
                if (csi_countdown) {
                    csi_countdown -= 1;
                    continue;
                }

/*                 ASSERT(incomplete_csi.size() == 0, "incomplete in middle of stream"); */

                p = &git->c;
                c = *p;

                if (yed_get_glyph_len(git) > 1) { goto put_utf8; }

                if (dectst) {
                    switch (c) {
                        case '8':
                            this->reset();
                            for (int row = 1; row <= this->height(); row += 1) {
                                for (int col = this->width(); col >= this->col(); col -= 1) {
                                    this->set_cell(row, col, GLYPH("E"));
                                }
                            }
                            break;
                    }
                    dectst = 0;
                    goto next;
                } else if (setcharset) {
                    /* Ignore character set setting. */
                    setcharset = 0;
                    goto next;
                }

                if (last.c == '\e') {
                    if (c != '[' && c != ']') {
                        if (!isprint(c)) {
                            DBG("ESC 0x%x", c);
                        } else {
                            DBG("ESC %c", c);
                        }
                    }

                    switch (c) {
                        case '\\':
                            /* String terminator. */
                            break;
                        case '[': {
                            CSI csi(p + 1);

                            DUMP_DEBUG();

                            if (csi.complete) {
                                DBG("CSI: '\\e[%.*s'", csi.len, p + 1);
                                this->execute_CSI(csi);
                                csi_countdown = csi.len;
                            } else {
                                if (*(p + 1 + csi.len) == 0) {
                                    DBG("INCOMPLETE CSI: '\\e[%.*s'", csi.len, p + 1);
                                    incomplete_csi.clear();
                                    incomplete_csi += "\e[";
                                    for (int i = 0; i < csi.len; i += 1) {
                                        incomplete_csi += *(p + 1 + i);
                                    }
                                } else {
                                    DBG("WARN: invalid/incomplete CSI: '\\e[%.*s'", csi.len, p + 1);
                                }

                                csi_countdown = incomplete_csi.size();
                            }

                            break;
                        }
                        case ']': {
                            OSC osc(p + 1);
                            csi_countdown = osc.len;

                            DUMP_DEBUG();

                            if (osc.complete) {
                                DBG("OSC: '\\e]%.*s'", osc.len, p + 1);
                                this->execute_OSC(osc);
                            } else {
                                if (*(p + 1 + osc.len) == 0) {
                                    DBG("INCOMPLETE OSC: '\\e]%.*s'", osc.len, p + 1);
                                    incomplete_csi.clear();
                                    incomplete_csi += "\e]";
                                    for (int i = 0; i < osc.len; i += 1) {
                                        incomplete_csi += *(p + 1 + i);
                                    }
                                } else {
                                    DBG("WARN: invalid/incomplete OSC: '\\e]%.*s'", osc.len, p + 1);
                                }

                                csi_countdown = incomplete_csi.size();
                            }

                            break;
                        }
                        case 'k':
                        case 'P': {
                            /* Device Control String */
                            DCS dcs(p + 1);
                            csi_countdown = dcs.len;

                            DUMP_DEBUG();

                            if (dcs.complete) {
                                DBG("DCS: '\\eP%.*s'", dcs.len, p + 1);
                            } else {
                                DBG("INCOMPLETE DCS: '\\eP%.*s'", dcs.len, p + 1);
                                incomplete_csi.clear();
                                incomplete_csi += "\eP";
                                for (int i = 0; i < dcs.len; i += 1) {
                                    incomplete_csi += *(p + 1 + i);
                                }
                            }

                            break;
                        }
                        case '#':
                            dectst = 1;
                            break;
                        case '=':
                            /* Ignore DECKPAM. */
                            break;
                        case '>':
                            /* Ignore DECKPNM. */
                            break;
                        case '(':
                            setcharset = 1;
                            break;
                        case '7':
                            this->save_cursor();
                            break;
                        case '8':
                            if (this->cursor_saved()) {
                                this->restore_cursor();
                            } else {
                                this->set_cursor(1, 1);
                                this->current_attrs = ZERO_ATTR;
                            }
                            break;
                        case 'D':
                        case 'E':
                            if (this->row() == this->scbottom()) {
                                this->scroll_up();
                            } else {
                                this->move_cursor(1, 0);
                            }
                            if (c == 'E') {
                                this->set_cursor(this->row(), 1);
                            }
                            break;
                        case 'M':
                            if (this->row() == this->sctop()) {
                                this->scroll_down();
                            } else {
                                this->move_cursor(-1, 0);
                            }
                            break;
                        case 'g':
                            /* Flash */
                            break;
                        default:;
                            DBG("UNHANDLED ESC 0x%x", c);
                            goto put;
                    }
                    goto next;
                }

                if (do_log) {
                    if (isprint(c)) {
                        if (last.c != '\e') {
                            debug += c;
                        }
                    } else {
                        if (c && c != '\e') {
                            DUMP_DEBUG();

                            char pc = 0;
                            if (c <= 0x1F) {
                                pc = c | 0x40;
                                goto dbg_out;
                            }

                            switch (c) {
                                case 0x7F: pc = '?'; break;
                            }

dbg_out:;
                            debug += '^';
                            debug += pc;
                            DUMP_DEBUG();
                        }
                    }
                }

                switch (c) {
                    case 0:
                        break;
                    case '\e':
                        if (incomplete_csi.size()) { incomplete_csi.clear(); }
                        if (!*(p + 1)) { incomplete_csi = "\e"; }
                        break;
                    case '\r':
                        this->set_cursor(this->row(), 1);
                        break;
                    case CTRL_H:
                        this->move_cursor(0, -1);
                        break;
                    case '\f':
                    case '\v':
                    case '\n':
                        if (this->row() == this->scbottom()) {
                            this->scroll_up();
                        } else {
                            this->move_cursor(1, 0);
                        }
                        break;
                    case '\t':
                        do {
                            if (this->col() == this->width()) { break; }

                            this->set_current_cell(GLYPH(" "));
                            this->move_cursor(0, 1);
                        } while (this->col() % yed_get_tab_width() != 1);
                        break;
                    case CTRL_G:
                        /* Bell */
                        break;
                    case CTRL_O:
                        /* Switch to standard char set. Ignore. */
                        break;
                    default:
                    put:;
                        if (iscntrl(git->c)) { goto next; }
                    put_utf8:;
                        int len = yed_get_glyph_len(git);
                        if (len > 1) {
                            if (p + len >= &buff.back()) {
                                for (int i = 0; i < len; i += 1) {
                                    incomplete_utf8.push_back(p[i]);
                                }
                                goto next;
                            }

                            if (do_log) {
                                for (int i = 0; i < len; i += 1) {
                                    debug += git->bytes[i];
                                }
                            }
                        }

                        if (this->wrap_next) {
                            if (this->col() == this->width()) {
                                if (this->row() == this->scbottom()) {
                                    this->scroll_up();
                                } else {
                                    this->move_cursor(1, 0);
                                }
                                this->set_cursor(this->row(), 1);
                            }
                            this->wrap_next = 0;
                        }

                        this->set_current_cell(git);
                        if (this->col() == this->width() && this->auto_wrap) {
                            this->wrap_next = 1;
                        } else {
                            this->move_cursor(0, yed_get_glyph_width(git), /* cancel_wrap = */ 0);
                        }
                        break;
                }
next:;
                last = yed_glyph_copy(git);
            }
        }

        this->write_to_buffer();

        if (ys->active_frame && ys->active_frame->buffer == this->buffer) {
            this->set_cursor_in_frame(ys->active_frame);
        }
    }

    void keys(int len, int *keys) {
        for (int i = 0; i < len; i += 1) {
            int key = keys[i];
            if (IS_MOUSE(key)) {
                switch (MOUSE_BUTTON(key)) {
                    case MOUSE_WHEEL_UP:
                        key = ARROW_UP;
                        break;
                    case MOUSE_WHEEL_DOWN:
                        key = ARROW_DOWN;
                        break;
                    default:
                        continue;
                }
            }

            switch (key) {
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_RIGHT:
                case ARROW_LEFT:
                case HOME_KEY:
                case END_KEY: {
                    char chars[2] = { '\e', this->app_keys ? 'O' : '[' };
                    write(this->master_fd, chars, 2);
                    break;
                }
                default:
                    break;
            }
            switch (key) {
                case ARROW_UP:
                    write(this->master_fd, "A", 1);
                    break;
                case ARROW_DOWN:
                    write(this->master_fd, "B", 1);
                    break;
                case ARROW_RIGHT:
                    write(this->master_fd, "C", 1);
                    break;
                case ARROW_LEFT:
                    write(this->master_fd, "D", 1);
                    break;
                case DEL_KEY:
                    write(this->master_fd, "P", 1);
                    break;
                case HOME_KEY:
                    write(this->master_fd, "H", 1);
                    break;
                case END_KEY:
                    write(this->master_fd, "F", 1);
                    break;
                case PAGE_UP:
                    write(this->master_fd, "\e[5~", 4);
                    break;
                case PAGE_DOWN:
                    write(this->master_fd, "\e[6~", 4);
                    break;
                case SHIFT_TAB:
                    write(this->master_fd, "\e[Z", 3);
                    break;
                case FN1:
                    write(this->master_fd, "\eOP", 3);
                    break;
                case FN2:
                    write(this->master_fd, "\eOQ", 3);
                    break;
                case FN3:
                    write(this->master_fd, "\eOR", 3);
                    break;
                case FN4:
                    write(this->master_fd, "\eOS", 3);
                    break;
                case FN5:
                    write(this->master_fd, "\e[15~", 5);
                    break;
                case FN6:
                    write(this->master_fd, "\e[17~", 5);
                    break;
                case FN7:
                    write(this->master_fd, "\e[18~", 5);
                    break;
                case FN8:
                    write(this->master_fd, "\e[19~", 5);
                    break;
                case FN9:
                    write(this->master_fd, "\e[20~", 5);
                    break;
                case FN10:
                    write(this->master_fd, "\e[21~", 5);
                    break;
                case FN11:
                    write(this->master_fd, "\e[23~", 5);
                    break;
                case FN12:
                    write(this->master_fd, "\e[24~", 5);
                    break;
                case MENU_KEY:
                    write(this->master_fd, "\e[29~", 5);
                    break;
                default:
                    char c = key;
                    write(this->master_fd, &c, 1);
            }
        }
    }

    void paste(const char *bytes) {
        write(this->master_fd, "\e[200~", 6);
        write(this->master_fd, bytes, strlen(bytes));
        write(this->master_fd, "\e[201~", 6);
    }

    void fit_to_frames() {
        int         in_frame;
        int         width;
        int         height;
        yed_frame **fit;
        yed_frame  *f;

        in_frame = 0;
        width    = INT_MAX;
        height   = INT_MAX;

        array_traverse(ys->frames, fit) {
            f = *fit;
            if (f->buffer != this->buffer) { continue; }

            in_frame = 1;

            if (f->width < width || f->height < height) {
                width  = f->width;
                height = f->height;
            }
        }

        if (in_frame) {
            this->resize(width, height);

            array_traverse(ys->frames, fit) {
                f = *fit;
                if (f->buffer == this->buffer) {
                    this->set_cursor_in_frame(f);
                }
            }
        }
    }

    void apply_attrs(yed_event *event) {
        yed_frame *frame;
        int        row;

        frame = event->frame;
        row   = event->row;

        for (int col = 1; col <= this->width(); col += 1) {
            auto &line = this->screen()[row - 1];

            if (col > line.size()) { break; }

            yed_attrs attrs = line[col - 1].attrs;

            if (ATTR_FG_KIND(attrs.flags) == ATTR_KIND_16 && attrs.fg >= 30 && attrs.fg <= 37) {
                int fg = attrs.fg - 30 + (!!(attrs.flags & ATTR_16_LIGHT_FG)) * 8;
                ATTR_SET_FG_KIND(attrs.flags, ATTR_FG_KIND(colors[fg].flags));
                attrs.fg = colors[fg].fg;
            }

            if (ATTR_BG_KIND(attrs.flags) == ATTR_KIND_16 && attrs.bg >= 30 && attrs.bg <= 37) {
                int bg = attrs.bg - 30 + (!!(attrs.flags & ATTR_16_LIGHT_BG)) * 8;
                ATTR_SET_BG_KIND(attrs.flags, ATTR_FG_KIND(colors[bg].flags));
                attrs.bg = colors[bg].fg;
            }

            yed_eline_combine_col_attrs(event, col, &attrs);
        }
    }

    void toggle_term_mode() {
        this->term_mode = !this->term_mode;
        if (this->term_mode
        &&  this->buffer
        &&  this->buffer->has_selection) {

            this->buffer->has_selection = 0;
        }
    }
};

struct Binding {
    int    len;
    int    keys[MAX_SEQ_LEN];
    char  *cmd;
    int    key;
    int    n_args;
    char **args;
};

struct State {
    u32                       term_counter = 0;
    std::list<Term*>          terms;
    std::map<yed_frame*, int> save_scroll_offsets;
    std::map<yed_frame*, int> term_mode_off;
    int                       key_sequences_saved = 0;
    array_t                   key_sequences;
    std::vector<Binding>      bindings;

    State() { }

    Term * new_term() {
        Term *t = new Term(this->term_counter);
        if (!t->valid) { return NULL; }

        this->term_counter += 1;
        this->terms.push_back(t);

        return t;
    }

    Term * new_term(int num) {
        Term *t = new Term(num);
        if (!t->valid) { return NULL; }

        this->terms.push_back(t);

        return t;
    }
};

#define STATE_ADDR_VAR_NAME "__term_state_addr"

static State      *state;
static yed_plugin *Self;

static Term * term_for_buffer(yed_buffer *buffer) {
    for (auto t : state->terms) {
        if (buffer == t->buffer) { return t; }
    }
    return NULL;
}

static void install_bindings() {
    for (auto &b : state->bindings) {
        if (b.len > 1) {
            b.key = yed_plugin_add_key_sequence(Self, b.len, b.keys);
        } else {
            b.key = b.keys[0];
        }
        yed_plugin_map_bind_key(Self, "terminal", b.key, b.cmd, b.n_args, b.args);
    }
    yed_enable_key_map("terminal");
}

static void uninstall_bindings() {
    yed_disable_key_map("terminal");

    for (auto &b : state->bindings) {
        yed_unbind_key(b.key);
        if (b.len > 1) {
            yed_delete_key_sequence(b.key);
        }
    }
}

static void update_bindings() {
    uninstall_bindings();
    install_bindings();
}

static void make_binding(int n_keys, int *keys, char *cmd, int n_args, char **args) {
    Binding binding;

    if (n_keys <= 0) {
        return;
    }

    binding.len = n_keys;
    for (int i = 0; i < n_keys; i += 1) {
        binding.keys[i] = keys[i];
    }
    binding.cmd = strdup(cmd);
    binding.key = KEY_NULL;
    binding.n_args = n_args;
    if (n_args) {
        binding.args = (char**)malloc(sizeof(char*) * n_args);
        for (int i = 0; i < n_args; i += 1) {
            binding.args[i] = strdup(args[i]);
        }
    } else {
        binding.args = NULL;
    }

    state->bindings.push_back(binding);

    if (state->key_sequences_saved) {
        update_bindings();
    }
}

static void del_binding(int n_keys, int *keys) {
    if (n_keys <= 0) {
        return;
    }

    int i = 0;
    for (auto &b : state->bindings) {
        if (b.len == n_keys
        &&  memcmp(b.keys, keys, n_keys * sizeof(int)) == 0) {
            break;
        }
        i += 1;
    }

    if (i == state->bindings.size()) { return; }

    if (state->key_sequences_saved) {
        uninstall_bindings();
    }

    state->bindings.erase(state->bindings.begin() + i);

    if (state->key_sequences_saved) {
        install_bindings();
    }
}

static void set_term_keys() {
    ASSERT(!state->key_sequences_saved, "key sequence save/restore mismatch");
    state->key_sequences       = ys->key_sequences;
    ys->key_sequences          = array_make(yed_key_sequence);
    state->key_sequences_saved = 1;
    install_bindings();
}

static void restore_normal_keys() {
    uninstall_bindings();
    ASSERT(state->key_sequences_saved, "key sequence save/restore mismatch");
    array_free(ys->key_sequences);
    ys->key_sequences          = state->key_sequences;
    state->key_sequences_saved = 0;
}

static void toggle_term_mode(Term *t) {
    if (t->term_mode) {
        restore_normal_keys();
    } else {
        set_term_keys();
    }

    t->toggle_term_mode();
}

static yed_direct_draw_t *term_mode_dd = NULL;
static void update(yed_event *event) {
    yed_frame *f;

    if (term_mode_dd != NULL) { yed_kill_direct_draw(term_mode_dd); term_mode_dd = NULL; }

    f = ys->active_frame;

    if (f != NULL) {
        if (auto t = term_for_buffer(f->buffer)) {
            if (!t->term_mode) {
                const char *s = " term-mode: OFF ";
                int row       = f->top + 1;
                int col       = f->left + f->width - 1 - strlen(s);
                auto attrs    = yed_parse_attrs("&red.fg &active.bg swap");
                term_mode_dd  = yed_direct_draw(row, col, attrs, s);
            }
        }
    }

    for (auto it = state->terms.begin(); it != state->terms.end(); it++) {
        Term *t = *it;

        if (t->process_exited) {
            if (t->bad_shell) {
                LOG_CMD_ENTER("yed-terminal");
                yed_cerr("Failed to start shell '%s'", get_shell());
                LOG_EXIT();
            }
            delete t;
            it = state->terms.erase(it);
            it--;
        } else {
            if (t->term_mode) {
                t->update();
            }
        }
    }
}

static void key(yed_event *event) {
    int len;
    int keys[MAX_SEQ_LEN];

    if (ys->interactive_command
    ||  !ys->active_frame) {
        return;
    }

    if (IS_MOUSE(event->key)) {
        auto btn = MOUSE_BUTTON(event->key);
        if (btn != MOUSE_WHEEL_UP && btn != MOUSE_WHEEL_DOWN) {
            return;
        }
    }

    auto t = term_for_buffer(ys->active_frame->buffer);
    if (t == NULL || !t->term_mode) { return; }

    for (auto &b : state->bindings) {
        if (b.key == event->key) { return; }
    }

    if (yed_var_is_truthy("terminal-debug-log")) {
        char *s = yed_keys_to_string(1, &event->key);
        DBG("KEY %s", s);
    }

    if (yed_get_real_keys(event->key, &len, keys)) {
        t->keys(len, keys);
        event->cancel = 1;
    }
}

static void ins(yed_event *event) {
    const char *text      = NULL;
    int         free_text = 0;

    if (ys->active_frame == NULL || ys->active_frame->buffer == NULL) { return; }

    if (auto t = term_for_buffer(ys->active_frame->buffer)) {
        if (strcmp(event->cmd_name, "simple-insert-string") == 0) {
            text = event->args[0];
        } else if (strcmp(event->cmd_name, "paste-yank-buffer") == 0) {
            text      = yed_get_buffer_text(yed_get_yank_buffer());
            free_text = 1;
        } else {
            return;
        }

        if (!t->term_mode) { toggle_term_mode(t); }

        if (text == NULL) { goto out; }

        t->paste(text);
        if (free_text) { free((char*)text); }
out:;
        event->cancel = 1;
    }
}

static void line(yed_event *event) {
    yed_frame  *frame;
    yed_buffer *buff;

    frame = event->frame;
    if (frame == NULL) { return; }

    buff = frame->buffer;
    if (buff == NULL) { return; }

    if (auto t = term_for_buffer(buff)) {
        t->apply_attrs(event);
    }
}

static void row(yed_event *event) {
    yed_frame  *frame;
    yed_buffer *buff;

    frame = event->frame;
    if (frame == NULL) { return; }

    buff = frame->buffer;
    if (buff == NULL) { return; }

    if (auto t = term_for_buffer(buff)) {
        event->row_base_attr = frame == ys->active_frame
                                    ? colors[CDEFAULT]
                                    : colors[CDEFAULT_INACTIVE];
    }
}


static void fit(yed_event *event) {
    static int zero_scroll_offset;

    if (event->kind == EVENT_FRAME_PRE_SET_BUFFER) {
        zero_scroll_offset = 0;

        if (!term_for_buffer(event->buffer)
        &&  term_for_buffer(event->frame->buffer)) {
            if (state->save_scroll_offsets.count(event->frame)) {
                event->frame->scroll_off = state->save_scroll_offsets[event->frame];
                state->save_scroll_offsets.erase(event->frame);
            }
        } else if (term_for_buffer(event->buffer)) {
            zero_scroll_offset = 1;
        }
    } else if (event->kind == EVENT_FRAME_POST_SET_BUFFER) {
        if (zero_scroll_offset) {
            state->save_scroll_offsets[event->frame] = event->frame->scroll_off;
            event->frame->scroll_off = 0;
        }
    }

    for (auto t : state->terms) {
        t->fit_to_frames();
    }
}

static void sig(yed_event *event) {
    int status;

    if (event->signum != SIGCHLD) { return; }

    for (auto t : state->terms) {
        if (waitpid(t->shell_pid, &status, WNOHANG)) {
            if (WIFEXITED(status)) {
                t->process_exited = 1;
                if (WEXITSTATUS(status) == 123) {
                    t->bad_shell = 1;
                }
                break;
            }
        }
    }
}

static void activated(yed_event *event) {
    auto t = term_for_buffer(event->frame->buffer);
    if (t == NULL) { return; }

    if (!t->term_mode
    &&  yed_var_is_truthy("terminal-auto-term-mode")) {
        toggle_term_mode(t);
    }
}

static void focus(yed_event *event) {
    int         from_term = 0;
    int         to_term   = 0;
    yed_buffer *to_buff   = NULL;

    if (event->kind == EVENT_FRAME_PRE_SET_BUFFER) {
        if (event->frame == ys->active_frame) {
            if (auto t = term_for_buffer(event->frame->buffer)) {
                if (t->term_mode) {  from_term = 1; }
            }
        }

        to_buff = event->buffer;
    } else if (event->kind == EVENT_FRAME_PRE_ACTIVATE) {
        if (event->frame == ys->active_frame) { return; }
        if (ys->active_frame != NULL) {
            if (auto t = term_for_buffer(ys->active_frame->buffer)) {
                if (t->term_mode) {  from_term = 1; }
            }
        }

        to_buff = event->frame->buffer;
    }

    if (auto t = term_for_buffer(to_buff)) {
        if (t->term_mode) {  to_term = 1; }
    }

    if (from_term != to_term) {
        if (to_term) {
            to_buff->has_selection = 0;
            set_term_keys();
        } else {
            restore_normal_keys();
        }
    }
}

static void parse_color(int which, const char *str) {
    ASSERT(which >= 0 && which <= N_COLORS, "invalid color index");
    colors[which] = str == NULL ? ZERO_ATTR : yed_parse_attrs(str);
}

static void var(yed_event *event) {
    const char *start     = "terminal-color";
    int         start_len = strlen("terminal-color");
    int         which     = -1;

    if (event->var_name == NULL || event->var_val == NULL) { return; }
    if (strncmp(event->var_name, start, start_len) != 0)   { return; }

    if (strcmp(event->var_name, "terminal-color-default") == 0) {
        which = CDEFAULT;
    } else if (strcmp(event->var_name, "terminal-color-default-inactive") == 0) {
        which = CDEFAULT_INACTIVE;
    } else {
        which = s_to_i(event->var_name + start_len);

        if (which < 0 || which > 15) { return; }
    }

    parse_color(which, event->var_val);
}

static void update_colors(void) {
    parse_color(0,                 yed_get_var("terminal-color0"));
    parse_color(1,                 yed_get_var("terminal-color1"));
    parse_color(2,                 yed_get_var("terminal-color2"));
    parse_color(3,                 yed_get_var("terminal-color3"));
    parse_color(4,                 yed_get_var("terminal-color4"));
    parse_color(5,                 yed_get_var("terminal-color5"));
    parse_color(6,                 yed_get_var("terminal-color6"));
    parse_color(7,                 yed_get_var("terminal-color7"));
    parse_color(8,                 yed_get_var("terminal-color8"));
    parse_color(9,                 yed_get_var("terminal-color9"));
    parse_color(10,                yed_get_var("terminal-color10"));
    parse_color(11,                yed_get_var("terminal-color11"));
    parse_color(12,                yed_get_var("terminal-color12"));
    parse_color(13,                yed_get_var("terminal-color13"));
    parse_color(14,                yed_get_var("terminal-color14"));
    parse_color(15,                yed_get_var("terminal-color15"));
    parse_color(CDEFAULT,          yed_get_var("terminal-color-default"));
    parse_color(CDEFAULT_INACTIVE, yed_get_var("terminal-color-default-inactive"));
}

static void style(yed_event *event) {
    update_colors();
}

static void term_new_cmd(int n_args, char **args) {
    Term *t = state->new_term();
    yed_cprint("new terminal buffer %s", t->buffer->name);
}

static void _term_open_cmd(int n_args, char **args, int newframe) {
    if (n_args > 1) {
        yed_cerr("expected 0 or 1 arguments, but got %d", n_args);
        return;
    }

    Term *t = NULL;
    if (n_args) {
        char buff[32];
        int num = s_to_i(args[0]);

        snprintf(buff, sizeof(buff), "*term%d", num);

        if (yed_buffer *buffer = yed_get_buffer(buff)) {
            t = term_for_buffer(buffer);
            if (t == NULL) {
                yed_cerr("*term%s is not a terminal buffer", args[0]);
                return;
            }
        } else {
            t = state->new_term(num);
        }
    } else {
        t = state->new_term();
    }

    if (newframe) {
        YEXE("special-buffer-prepare-focus", t->buffer->name);
    }

    YEXE("buffer", t->buffer->name);
}

static void term_open_cmd(int n_args, char **args) {
    _term_open_cmd(n_args, args, 1);
}

static void term_open_no_frame_cmd(int n_args, char **args) {
    _term_open_cmd(n_args, args, 0);
}

static void term_feed_keys_cmd(int n_args, char **args) {
    yed_buffer *buffer;
    int         n;
    int         keys[MAX_SEQ_LEN];

    if (n_args < 2) {
        yed_cerr("expected 2 or more arguments, but got %d", n_args);
        return;
    }

    buffer = yed_get_buffer(args[0]);
    if (buffer == NULL) {
        yed_cerr("unknown buffer '%s'", args[0]);
        return;
    }

    if (auto t = term_for_buffer(buffer)) {
        for (int i = 1; i < n_args; i += 1) {
            if ((n = yed_string_to_keys(args[i], keys)) > 0) {
                t->keys(n, keys);
            }
        }
    } else {
        yed_cerr("'%s' is not a terminal buffer", args[0]);
        return;
    }
}

static void term_feed_text_cmd(int n_args, char **args) {
    yed_buffer *buffer;
    int         key;

    if (n_args != 2) {
        yed_cerr("expected 2 arguments, but got %d", n_args);
        return;
    }

    buffer = yed_get_buffer(args[0]);
    if (buffer == NULL) {
        yed_cerr("unknown buffer '%s'", args[0]);
        return;
    }

    if (auto t = term_for_buffer(buffer)) {
        for (int i = 0; i < strlen(args[1]); i += 1) {
            key = args[1][i];

            switch (key) {
                case '\n':  key = ENTER; break;
                case '\t':  key = TAB;   break;
            }

            t->keys(1, &key);
        }
    } else {
        yed_cerr("'%s' is not a terminal buffer", args[0]);
        return;
    }
}

static void toggle_term_mode_cmd(int n_args, char **args) {
    if (ys->active_frame == NULL) {
        yed_cerr("no active frame");
        return;
    }

    if (auto t = term_for_buffer(ys->active_frame->buffer)) {
        toggle_term_mode(t);
    } else {
        yed_cerr("active frame does not have a terminal buffer in it");
        return;
    }
}

static void term_mode_off_cmd(int n_args, char **args) {
    yed_buffer *buffer;

    if (n_args != 1) {
        yed_cerr("expected 1 argument, but got %d", n_args);
        return;
    }

    buffer = yed_get_buffer(args[0]);
    if (buffer == NULL) {
        yed_cerr("unknown buffer '%s'", args[0]);
        return;
    }

    if (auto t = term_for_buffer(buffer)) {
        if (t->term_mode) {
            toggle_term_mode(t);
        }
    } else {
        yed_cerr("'%s' is not a terminal buffer", args[0]);
        return;
    }
}

static void term_mode_on_cmd(int n_args, char **args) {
    yed_buffer *buffer;

    if (n_args != 1) {
        yed_cerr("expected 1 argument, but got %d", n_args);
        return;
    }

    buffer = yed_get_buffer(args[0]);
    if (buffer == NULL) {
        yed_cerr("unknown buffer '%s'", args[0]);
        return;
    }

    if (auto t = term_for_buffer(buffer)) {
        if (!t->term_mode) {
            toggle_term_mode(t);
        }
    } else {
        yed_cerr("'%s' is not a terminal buffer", args[0]);
        return;
    }
}

static void term_bind_cmd(int n_args, char **args) {
    char            *cmd, **cmd_args;
    int              n_keys, keys[MAX_SEQ_LEN], n_cmd_args;

    if (n_args == 0) {
        yed_cerr("missing 'keys' as first argument");
        return;
    }

    if (n_args < 2) {
        yed_cerr("missing 'command', 'command_args'... as second and up arguments");
        return;
    }

    n_keys = yed_string_to_keys(args[0], keys);
    if (n_keys == -1) {
        yed_cerr("invalid string of keys '%s'", args[0]);
        return;
    }
    if (n_keys == -2) {
        yed_cerr("too many keys to be a sequence in '%s'", args[0]);
        return;
    }

    cmd        = args[1];
    n_cmd_args = n_args - 2;
    cmd_args   = args + 2;

    make_binding(n_keys, keys, cmd, n_cmd_args, cmd_args);
}

static void term_unbind_cmd(int n_args, char **args) {
    int n_keys, keys[MAX_SEQ_LEN];

    if (n_args != 1) {
        yed_cerr("expected 'keys' as first and only argument");
        return;
    }

    n_keys = yed_string_to_keys(args[0], keys);
    if (n_keys == -1) {
        yed_cerr("invalid string of keys '%s'", args[0]);
        return;
    }
    if (n_keys == -2) {
        yed_cerr("too many keys to be a sequence in '%s'", args[0]);
        return;
    }

    del_binding(n_keys, keys);
}

static void unload(yed_plugin *self) {
    if (term_mode_dd != NULL) {
        yed_kill_direct_draw(term_mode_dd);
        term_mode_dd = NULL;
    }
}

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    char *state_addr_str;
    char  addr_buff[64];

    YED_PLUG_VERSION_CHECK();

    Self = self;

    if ((state_addr_str = yed_get_var(STATE_ADDR_VAR_NAME))) {
        sscanf(state_addr_str, "%p", (void**)&state);
    } else {
        state = new State;
        snprintf(addr_buff, sizeof(addr_buff), "%p", (void*)state);
        yed_set_var(STATE_ADDR_VAR_NAME, addr_buff);
    }

    std::map<void(*)(yed_event*), std::vector<yed_event_kind_t> > event_handlers = {
        { update,    { EVENT_PRE_DRAW_EVERYTHING                            } },
        { key,       { EVENT_KEY_PRESSED                                    } },
        { ins,       { EVENT_CMD_PRE_RUN                                    } },
        { line,      { EVENT_LINE_PRE_DRAW                                  } },
        { row,       { EVENT_ROW_PRE_CLEAR                                  } },
        { fit,       { EVENT_FRAME_POST_RESIZE, EVENT_TERMINAL_RESIZED,
                       EVENT_FRAME_POST_DELETE, EVENT_FRAME_PRE_SET_BUFFER,
                       EVENT_FRAME_POST_SET_BUFFER,                         } },
        { sig,       { EVENT_SIGNAL_RECEIVED                                } },
        { activated, { EVENT_FRAME_ACTIVATED                                } },
        { focus,     { EVENT_FRAME_PRE_SET_BUFFER, EVENT_FRAME_PRE_ACTIVATE } },
        { var,       { EVENT_VAR_POST_SET                                   } },
        { style,     { EVENT_STYLE_CHANGE                                   } }};

    std::map<const char*, const char*> vars = {
        { "terminal-debug-log",              "OFF"                         },
        { "terminal-shell",                  get_shell()                   },
        { "terminal-termvar",                get_termvar()                 },
        { "terminal-scrollback",             XSTR(DEFAULT_SCROLLBACK)      },
        { "terminal-max-block-size",         XSTR(DEFAULT_MAX_BLOCK_SIZE)  },
        { "terminal-read-chunk-size",        XSTR(DEFAULT_READ_CHUNK_SIZE) },
        { "terminal-auto-term-mode",         "ON"                          },
        { "terminal-show-welcome",           "yes"                         },
        { "terminal-color0",                 "&black"                      },
        { "terminal-color1",                 "&red"                        },
        { "terminal-color2",                 "&green"                      },
        { "terminal-color3",                 "&yellow"                     },
        { "terminal-color4",                 "&blue"                       },
        { "terminal-color5",                 "&magenta"                    },
        { "terminal-color6",                 "&cyan"                       },
        { "terminal-color7",                 "&gray"                       },
        { "terminal-color8",                 "&gray"                       },
        { "terminal-color9",                 "&red"                        },
        { "terminal-color10",                "&green"                      },
        { "terminal-color11",                "&yellow"                     },
        { "terminal-color12",                "&blue"                       },
        { "terminal-color13",                "&magenta"                    },
        { "terminal-color14",                "&cyan"                       },
        { "terminal-color15",                "&white"                      },
        { "terminal-color-default",          "&active"                     },
        { "terminal-color-default-inactive", "&inactive"                   }};

    std::map<const char*, void(*)(int, char**)> cmds = {
        { "term-new",           term_new_cmd           },
        { "term-open",          term_open_cmd          },
        { "term-open-no-frame", term_open_no_frame_cmd },
        { "term-feed-keys",     term_feed_keys_cmd     },
        { "term-feed-text",     term_feed_text_cmd     },
        { "term-bind",          term_bind_cmd          },
        { "term-unbind",        term_unbind_cmd        },
        { "term-mode-off",      term_mode_off_cmd      },
        { "term-mode-on",       term_mode_on_cmd       },
        { "toggle-term-mode",   toggle_term_mode_cmd   }};

    for (auto &pair : event_handlers) {
        for (auto evt : pair.second) {
            yed_event_handler h;
            h.kind = evt;
            h.fn   = pair.first;
            yed_plugin_add_event_handler(self, h);
        }
    }

    for (auto &pair : vars) {
        if (!yed_get_var(pair.first)) { yed_set_var(pair.first, pair.second); }
    }

    for (auto &pair : cmds) {
        yed_plugin_set_command(self, pair.first, pair.second);
    }

    yed_plugin_add_key_map(self, "terminal");
    update_colors();

    YEXE("term-bind", "ctrl-t", "toggle-term-mode");

    yed_plugin_set_unload_fn(self, unload);

    return 0;
}
