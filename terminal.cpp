#include <memory>
#include <vector>
#include <list>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
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



/* #define DBG_LOG_ON */

#define LOG__XSTR(x) #x
#define LOG_XSTR(x) LOG__XSTR(x)

#define LOG(...)                                               \
do {                                                           \
    LOG_FN_ENTER();                                            \
    yed_log(__VA_ARGS__);                                      \
    LOG_EXIT();                                                \
} while (0)

#define ELOG(...)                                              \
do {                                                           \
    LOG_FN_ENTER();                                            \
    yed_log("[!] " __VA_ARGS__);                               \
    LOG_EXIT();                                                \
} while (0)

#ifdef DBG_LOG_ON
#define DBG(...)                                               \
do {                                                           \
    LOG_FN_ENTER();                                            \
    yed_log(__FILE__ ":" LOG_XSTR(__LINE__) ": " __VA_ARGS__); \
    LOG_EXIT();                                                \
} while (0)
#else
#define DBG(...) ;
#endif

#define BUFF_WRITABLE_GUARD(_buff)             \
    (_buff)->flags &= ~(BUFF_RD_ONLY);         \
    defer { (_buff)->flags |= BUFF_RD_ONLY; };



struct CSI {
    std::vector<long> args;
    int               command  = 0;
    int               len      = 0;
    int               complete = 0;
    int               priv     = 0;

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

#define IS_DELIM(_c) ((_c) == ';' || (_c) == ':' || (_c) == '?' || (_c) == ' ')
#define IS_FINAL(_c) ((_c) >= 0x40 && (_c) <= 0x7E)

        while (IS_DELIM(c)) {
            if (c == '?')      { this->priv = 1;          }
            else if (c == ';') { this->args.push_back(0); }
            NEXT();
        }

        while (!IS_FINAL(c)) {
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

        if (c != ';') { goto out; }

        while (c && c != CTRL_G) {
            if (c == '\e') {
                NEXT();
                if (c == '\\') { break; }
            }
            this->arg += c;
            NEXT();
        }

        this->complete = 1;
out:;

#undef NEXT
    }
};

struct Cell {
    yed_glyph glyph = G(0);
    yed_attrs attrs = ZERO_ATTR;

    Cell() { }

    Cell(yed_glyph g, yed_attrs a) {
        this->glyph = g;
        this->attrs = a;
    }
};

struct Line : std::vector<Cell> {
    int dirty = 0;

    void clear_cells(int width) {
        this->resize(width);
        std::fill(this->begin(), this->end(), Cell());
    }

    Line(int width) {
        this->clear_cells(width);
    }
};

#define DEFAULT_WIDTH           (80)
#define DEFAULT_HEIGHT          (24)
#define DEFAULT_SCROLLBACK      (10000)

struct Screen : std::vector<Line> {
    int width      = 0;
    int height     = 0;
    int cursor_row = 1;
    int cursor_col = 1;
    int scroll_t   = 0;
    int scroll_b   = 0;
    int scrollback = DEFAULT_SCROLLBACK;

    Screen() { }

    void set_cursor(int row, int col) {
        this->cursor_row = row; LIMIT(this->cursor_row, 1, this->height);
        this->cursor_col = col; LIMIT(this->cursor_col, 1, this->width);
    }

    void move_cursor(int rows, int cols) {
        this->set_cursor(this->cursor_row + rows, this->cursor_col + cols);
    }

    void make_line_dirty_abs(int row) {
        auto &line = (*this)[row - 1];
        line.dirty = 1;
    }

    void make_dirty() {
        for (auto &line : *this) { line.dirty = 1; }
    }

    void set_dimensions(int width, int height) {
        int num_lines;
        int max_width;

        num_lines = height + this->scrollback;

        if (num_lines > this->size()) {
            this->resize(num_lines, Line(width));
        } else {
            while (this->size() > num_lines) {
                this->erase(this->begin());
            }
        }

        max_width = width;
        for (auto &line : *this) {
            if (line.size() > max_width) {
                max_width = line.size();
            }
        }

        if (max_width > this->width) {
            for (auto &line : *this) {
                line.resize(max_width, Cell());
            }
        }

        this->width  = width;
        this->height = height;

        LIMIT(this->scroll_t, 0, this->height);
        LIMIT(this->scroll_b, 0, this->height);

        this->make_dirty();
    }

    void set_scroll(int top, int bottom) {
        this->scroll_t = top;    LIMIT(this->scroll_t, 0, this->height);
        this->scroll_b = bottom; LIMIT(this->scroll_b, 0, this->height);
    }

    int sctop()    { return this->scroll_t ? this->scroll_t : 1;            }
    int scbottom() { return this->scroll_b ? this->scroll_b : this->height; }

    void set(int row, int col, yed_glyph g, yed_attrs a) {
        if (row > this->height || col > this->width) { return; }

        auto &line = (*this)[this->scrollback + row - 1];
        auto &cell = line[col - 1];

        cell.glyph = g;

        int width = yed_get_glyph_width(g);
        for (int i = 0; i < width; i += 1) {
            if (col + i > this->width) { break; }
            auto &cell = line[col - 1 + i];
            cell.attrs = a;
        }

        line.dirty = 1;
    }

    void set_current_cell(yed_glyph g, yed_attrs a) {
        this->set(this->cursor_row, this->cursor_col, g, a);
    }

    void del_cell(int row, int col) {
        auto &line = (*this)[this->scrollback + row - 1];
        line.erase(line.begin() + col - 1);
        line.emplace_back();
    }

    void clear_row_abs(int row) {
        auto &line = (*this)[row - 1];
        line.clear_cells(this->width);
        line.dirty = 1;
    }

    void clear_row(int row) {
        this->clear_row_abs(this->scrollback + row);
    }

    void scroll_up(yed_buffer *buffer) {
        int del_row = this->scroll_t ? this->scrollback + this->scroll_t : 1;
        int new_row = this->scrollback + this->scbottom();

        this->erase(this->begin() + del_row - 1);
        if (new_row > this->size()) {
            this->emplace_back(this->width);
        } else {
            this->emplace(this->begin() + new_row - 1, this->width);
        }

        this->make_line_dirty_abs(new_row);

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

        ASSERT(this->size() == this->scrollback + this->height, "rows mismatch");
    }

    void scroll_down(yed_buffer *buffer) {
        int del_row = this->scrollback + this->scbottom();
        int new_row = this->scroll_t ? this->scrollback + this->scroll_t : 1;

        this->erase(this->begin() + del_row - 1);
        this->emplace(this->begin() + new_row - 1, this->width);
        this->make_line_dirty_abs(new_row);

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

        ASSERT(this->size() == this->scrollback + this->height, "rows mismatch");
    }

    void insert_line(int row, yed_buffer *buffer) {
        int del_row = this->scrollback + this->scbottom();
        int new_row = this->scrollback + row;

        this->erase(this->begin() + del_row - 1);
        if (new_row > this->size()) {
            this->emplace_back(this->width);
        } else {
            this->emplace(this->begin() + new_row - 1, this->width);
        }

        this->make_line_dirty_abs(new_row);

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

        ASSERT(this->size() == this->scrollback + this->height, "rows mismatch");
    }

    void delete_line(int row, yed_buffer *buffer) {
        int del_row = this->scrollback + row;
        int new_row = this->scrollback + this->scbottom();

        this->erase(this->begin() + del_row - 1);
        this->emplace(this->begin() + new_row - 1, this->width);
        this->make_line_dirty_abs(new_row);

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

        ASSERT(this->size() == this->scrollback + this->height, "rows mismatch");
    }

    void write_to_buffer(yed_buffer *buffer) {
        BUFF_WRITABLE_GUARD(buffer);

        int row = 1;
        for (auto &line : *this) {
            if (line.dirty) {
                int n = line.size();

                for (; n >= 1; n -= 1) {
                    if (line[n - 1].glyph.c != 0) { break; }
                }

                yed_line_clear_no_undo(buffer, row);
                for (int i = 0; i < n; i += 1) {
                    auto g = line[i].glyph.c ? line[i].glyph : G(' ');
                    yed_append_to_line_no_undo(buffer, row, g);
                }

                line.dirty = 0;
            }
            row += 1;
        }
    }
};

#define DEFAULT_SHELL   "/bin/bash"
#define DEFAULT_TERMVAR "screen"

const char *get_shell() {
    const char *shell;

    shell = yed_get_var("terminal-shell");

    if (shell == NULL) { shell = getenv("SHELL"); }
    if (shell == NULL) { shell = DEFAULT_SHELL;   }

    return shell;
}

const char *get_termvar() {
    return DEFAULT_TERMVAR;
}


struct Term {
    int                valid          = 0;
    int                master_fd      = 0;
    int                slave_fd       = 0;
    pid_t              shell_pid      = 0;
    int                process_exited = 0;
    int                bad_shell      = 0;
    std::thread        thr;
    std::mutex         buff_lock;
    std::vector<char>  data_buff;
    yed_buffer        *buffer         = NULL;
    yed_attrs          current_attrs  = ZERO_ATTR;
    Screen             main_screen;
    Screen             alt_screen;
    Screen            *_screen        = NULL;
    int                app_keys       = 0;
    std::string        title;

    static void read_thread(Term *term) {
        ssize_t n;
        char    buff[512];

        for (;;) {

            if ((n = read(term->master_fd, buff, sizeof(buff))) <= 0) {
                errno                = 0;
                term->process_exited = 1;
                break;
            }

            { std::lock_guard<std::mutex> lock(term->buff_lock);

                for (size_t i = 0; i < n; i += 1) {
                    term->data_buff.push_back(buff[i]);
                }
            }

            yed_force_update();
        }

        yed_force_update();
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

        this->main_screen.set_dimensions(width, height);
        this->alt_screen.set_dimensions(width, height);

        { BUFF_WRITABLE_GUARD(this->buffer);
            int n_rows = this->screen().size();
            if (yed_buff_n_lines(this->buffer) < n_rows) {
                while (yed_buff_n_lines(this->buffer) < n_rows) {
                    yed_buff_insert_line_no_undo(this->buffer, 1);
                }
            } else while (yed_buff_n_lines(this->buffer) > n_rows) {
                yed_buff_delete_line_no_undo(this->buffer, 1);
            }
        }

        ASSERT(yed_buff_n_lines(this->buffer) == this->screen().scrollback + height,
               "buff wrong size");

        DBG("new size %dx%d", width, height);
    }


    Term(u32 num) : _screen(&this->main_screen) {
        char           name[64];
        pid_t          p;
        struct winsize ws;

        snprintf(name, sizeof(name), "*term%u", num);
        this->buffer = yed_get_or_create_special_rdonly_buffer(name);

        ws.ws_row    = DEFAULT_HEIGHT;
        ws.ws_col    = DEFAULT_WIDTH;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;

        if (openpty(&this->master_fd, &this->slave_fd, NULL, NULL, &ws) != 0) {
            ELOG("openpty() failed with errno = %d", errno);
            errno = 0;
            return;
        }

        p = fork();
        if (p == 0) {
            close(this->master_fd);
            login_tty(this->slave_fd);

            setenv("TERM", get_termvar(), 1);

            printf("Wecome to\n\n");
            printf(TERM_CYAN);
            printf(
"██    ██ ███████ ██████      ████████ ███████ ██████  ███    ███ ██ ███    ██  █████  ██      \n"
" ██  ██  ██      ██   ██        ██    ██      ██   ██ ████  ████ ██ ████   ██ ██   ██ ██      \n"
"  ████   █████   ██   ██        ██    █████   ██████  ██ ████ ██ ██ ██ ██  ██ ███████ ██      \n"
"   ██    ██      ██   ██        ██    ██      ██   ██ ██  ██  ██ ██ ██  ██ ██ ██   ██ ██      \n"
"   ██    ███████ ██████         ██    ███████ ██   ██ ██      ██ ██ ██   ████ ██   ██ ███████ \n"
            );
            printf(TERM_RESET);
            printf("\n");

#if 0
            printf(
"Welcome to\n"
TERM_BLUE
"                _   _                      _             _ \n"
" _   _  ___  __| | | |_ ___ _ __ _ __ ___ (_)_ __   __ _| |\n"
"| | | |/ _ \\/ _` | | __/ _ \\ '__| '_ ` _ \\| | '_ \\ / _` | |\n"
"| |_| |  __/ (_| | | ||  __/ |  | | | | | | | | | | (_| | |\n"
" \\__, |\\___|\\__,_|  \\__\\___|_|  |_| |_| |_|_|_| |_|\\__,_|_|\n"
" |___/  \n\n" TERM_RESET);
#endif

            char * const args[] = { (char*)get_shell(), NULL };
            execvp(get_shell(), args);
            exit(123);
        } else {
            this->shell_pid = p;

            this->resize(DEFAULT_WIDTH, DEFAULT_HEIGHT);
            this->set_cursor(1, 1);

            thr = std::thread(Term::read_thread, this);

            this->valid = 1;
        }
    }

    ~Term() {
        close(this->master_fd);
        close(this->slave_fd);
        thr.join();
        yed_free_buffer(this->buffer);
    }

    void set_cursor(int row, int col)    { this->screen().set_cursor(row, col);    }
    void move_cursor(int rows, int cols) { this->screen().move_cursor(rows, cols); }
    void set_scroll(int top, int bottom) { this->screen().set_scroll(top, bottom); }

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
        this->set_scroll(0, 0);
        this->set_cursor(1, 1);
        this->clear_page();
    }

    void set_cell(int row, int col, yed_glyph g) {
        auto attrs = g.c ? this->current_attrs : ZERO_ATTR;
        this->screen().set(row, col, g, attrs);
    }

    void set_current_cell(yed_glyph g) {
        auto attrs = g.c ? this->current_attrs : ZERO_ATTR;
        this->screen().set_current_cell(g, attrs);
    }

    void delete_cell(int row, int col) {
        this->screen().del_cell(row, col);
    }

    void delete_current_cell() {
        this->delete_cell(this->row(), this->col());
    }

    void scroll_up()                   { this->screen().scroll_up(this->buffer);        }
    void scroll_down()                 { this->screen().scroll_down(this->buffer);      }
    void insert_line(int row)          { this->screen().insert_line(row, this->buffer); }
    void delete_line(int row)          { this->screen().delete_line(row, this->buffer); }

    void set_cursor_in_frame(yed_frame *frame) {
        yed_set_cursor_within_frame(frame, this->scrollback_row() + this->height(), this->col());
        yed_set_cursor_within_frame(frame, this->scrollback_row(), this->col());
    }

    void write_to_buffer() { this->screen().write_to_buffer(this->buffer); }

    void execute_CSI(CSI &csi) {
        long val;

#define PRIV_ENC(_cmd, _priv) ((_cmd) | ((_priv) << 31))
#define PRIV(_cmd)            PRIV_ENC((_cmd), 1)

#define SHIFT()                           \
do {                                      \
    if (csi.args.size()) {                \
        csi.args.erase(csi.args.begin()); \
    }                                     \
} while (0)

        switch (PRIV_ENC(csi.command, csi.priv)) {
            case '@':
                val = csi.args.size() ? csi.args[0] : 1;
                for (int i = 0; i < val; i += 1) {
                    this->move_cursor(0, 1);
                    this->set_current_cell(G(' '));
                }
                break;
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
            case 'C':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(0, val);
                break;
            case 'D':
                val = csi.args.size() ? csi.args[0] : 1;
                this->move_cursor(0, -val);
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
                            this->set_cell(this->row(), col, G(0));
                        }
                        break;
                    case 1:
                        for (int row = 1; row < this->row(); row += 1) {
                            this->clear_row(row);
                        }
                        for (int col = 1; col >= this->col(); col += 1) {
                            this->set_cell(this->row(), col, G(0));
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
                            this->set_cell(this->row(), col, G(0));
                        }
                        break;
                    case 1:
                        for (int col = 1; col <= this->col(); col += 1) {
                            this->set_cell(this->row(), col, G(0));
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
                    case 1049:
                        this->_screen = &this->alt_screen;
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
                            this->current_attrs.flags |= ATTR_16;
                            this->current_attrs.fg     = cmd;
                            break;
                        case 38: {
                            auto which = csi.args[0];
                            SHIFT();
                            switch (which) {
                                case 2: {
                                    auto r = csi.args[0]; SHIFT();
                                    auto g = csi.args[0]; SHIFT();
                                    auto b = csi.args[0]; SHIFT();
                                    this->current_attrs.flags &= ~(ATTR_16 | ATTR_256);
                                    this->current_attrs.flags |= ATTR_RGB;
                                    this->current_attrs.fg     = RGB_32(r, g, b);
                                    break;
                                }
                                case 5:
                                    this->current_attrs.flags &= ~(ATTR_16 | ATTR_RGB);
                                    this->current_attrs.flags |= ATTR_256;
                                    this->current_attrs.fg     = csi.args[0];
                                    SHIFT();
                                    break;
                            }
                            break;
                        }
                        case 39:
                            this->current_attrs.fg = 0;
                            break;
                        case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                            this->current_attrs.flags |= ATTR_16;
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
                                    this->current_attrs.flags &= ~(ATTR_16 | ATTR_256);
                                    this->current_attrs.flags |= ATTR_RGB;
                                    this->current_attrs.bg     = RGB_32(r, g, b);
                                    break;
                                }
                                case 5:
                                    this->current_attrs.flags &= ~(ATTR_16 | ATTR_RGB);
                                    this->current_attrs.flags |= ATTR_256;
                                    this->current_attrs.bg     = csi.args[0];
                                    SHIFT();
                                    break;
                            }
                            break;
                        }
                        case 49:
                            this->current_attrs.bg = 0;
                            break;

                        default:
                            goto unhandled;
                            break;
                    }
                }
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
            default:
                DBG("  UNRECOGNIZED OSC");
            unhandled:;
                DBG("  UNHANDLED OSC %ld", osc.command);
        }
    }

    void update() {
        std::vector<char>  buff;
        static std::string incomplete_csi;
        int                dectst = 0;

        { std::lock_guard<std::mutex> lock(this->buff_lock);

            buff = std::move(this->data_buff);
            this->data_buff.clear();
        }

        buff.push_back(0);

        if (incomplete_csi.size()) {
            for (auto it = incomplete_csi.rbegin(); it != incomplete_csi.rend(); it++) {
                buff.insert(buff.begin(), *it);
            }
            incomplete_csi.clear();
        }

#define DUMP_DEBUG()                \
do {                                \
    if (debug.size()) {             \
        DBG("'%s'", debug.c_str()); \
        debug.clear();              \
    }                               \
} while (0)

        std::string debug;

        { BUFF_WRITABLE_GUARD(this->buffer);

            char        *s             = buff.data();
            size_t       len           = buff.size() - 1;
            yed_glyph   *git           = NULL;
            yed_glyph    last          = G(0);
            int          csi_countdown = 0;

            yed_glyph_traverse_n(s, len, git) {
                if (csi_countdown) {
                    csi_countdown -= 1;
                    continue;
                }

                ASSERT(incomplete_csi.size() == 0, "incomplete in middle of stream");

                char *p = &git->c;
                char  c = *p;

                if (yed_get_glyph_len(*git) > 1) { goto put_utf8; }

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

                if (dectst) {
                    switch (c) {
                        case '8':
                            this->reset();
                            for (int row = 1; row <= this->height(); row += 1) {
                                for (int col = this->width(); col >= this->col(); col -= 1) {
                                    this->set_cell(row, col, G('E'));
                                }
                            }
                            break;
                        default:;
                            goto next;
                            break;
                    }
                }

                dectst = 0;

                if (last.c == '\e') {
                    switch (c) {
                        case '[': {
                            CSI csi(p + 1);

                            DUMP_DEBUG();

                            if (csi.complete) {
                                DBG("CSI: '\\e[%.*s'", csi.len, p + 1);
                                this->execute_CSI(csi);
                                csi_countdown = csi.len;
                            } else {
                                DBG("INCOMPLETE CSI: '\\e[%.*s'", csi.len, p + 1);
                                incomplete_csi = p - 1;
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
                                DBG("INCOMPLETE OSC: '\\e]%.*s'", osc.len, p + 1);
                                incomplete_csi.clear();
                                incomplete_csi += "\e]";
                                for (int i = 0; i < osc.len; i += 1) {
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
                        case '8':
                            this->set_cursor(1, 1);
                            this->current_attrs = ZERO_ATTR;
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
                            goto put;
                    }
                    goto next;
                }

                switch (c) {
                    case 0:
                        break;
                    case '\e':
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
                        this->set_cursor(this->row(), 1);
                        break;
                    case '\t':
                        do {
                            this->set_current_cell(G(' '));
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
                        if (yed_get_glyph_len(*git) > 1) {
                            for (int i = 0; i < yed_get_glyph_len(*git); i += 1) {
                                debug += git->bytes[i];
                            }
                        }
                        this->set_current_cell(*git);
                        this->move_cursor(0, yed_get_glyph_width(*git));
                        break;
                }
next:;
                last = *git;
            }
out:;
        }

        this->write_to_buffer();

        if (ys->active_frame && ys->active_frame->buffer == this->buffer) {
            this->set_cursor_in_frame(ys->active_frame);
        }
    }

    void keys(int len, int *keys) {
        for (int i = 0; i < len; i += 1) {
            switch (keys[i]) {
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_RIGHT:
                case ARROW_LEFT: {
                    char chars[2] = { '\e', this->app_keys ? 'O' : '[' };
                    write(this->master_fd, chars, 2);
                    break;
                }
                default:
                    break;
            }
            switch (keys[i]) {
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
                    write(this->master_fd, "\e[3~", 4);
                    break;
                case HOME_KEY:
                    write(this->master_fd, "\e[1~", 4);
                    break;
                case END_KEY:
                    write(this->master_fd, "\e[4~", 4);
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
                    char c = keys[i];
                    write(this->master_fd, &c, 1);
            }
        }
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

    void apply_attrs(yed_frame *frame, int row, array_t *line_attrs) {
        int        col;
        yed_attrs *ait;

        col = 1;
        array_traverse(*line_attrs, ait) {
            if (col > this->screen()[row - 1].size()) { break; }
            yed_attrs &attrs = this->screen()[row - 1][col - 1].attrs;

            *ait = attrs;

            col += 1;
        }
    }
};

struct State {
    u32                       term_counter = 0;
    std::list<Term*>          terms;
    std::map<yed_frame*, int> save_scroll_offsets;

    State() { }

    Term * new_term() {
        Term *t = new Term(this->term_counter);
        if (!t->valid) { return NULL; }

        this->term_counter += 1;
        this->terms.push_back(t);

        return t;
    }
};

#define STATE_ADDR_VAR_NAME "__term_state_addr"

static State *state;

static Term * term_for_buffer(yed_buffer *buffer) {
    for (auto t : state->terms) {
        if (buffer == t->buffer) { return t; }
    }
    return NULL;
}

static int term_mode;

static void toggle_term_mode(void) {
    term_mode = !term_mode;
}

static void update(yed_event *event) {
    if (!term_mode) { return; }

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
            t->update();
        }
    }
}

static void key(yed_event *event) {
    static int ignore;
    int        len;
    int        keys[MAX_SEQ_LEN];

    if (!term_mode
    ||  ys->interactive_command
    ||  ignore
    ||  !ys->active_frame
    ||  IS_MOUSE(event->key)) {

        ignore = 0;
        return;
    }

    if (yed_get_real_keys(event->key, &len, keys)) {
        if (auto t = term_for_buffer(ys->active_frame->buffer)) {
            if (event->key == CTRL_T) {
                toggle_term_mode();
                event->cancel = 1;
                return;
            }

            t->keys(len, keys);
            event->cancel = 1;
            return;
        }
    }

    event->cancel = 0;
}

static void line(yed_event *event) {
    yed_frame  *frame;
    yed_buffer *buff;

    frame = event->frame;
    if (frame == NULL) { return; }

    buff = frame->buffer;
    if (buff == NULL) { return; }

    if (auto t = term_for_buffer(buff)) {
        t->apply_attrs(event->frame, event->row, &event->line_attrs);
    }
}

static void row(yed_event *event) {
    yed_frame  *frame;
    yed_buffer *buff;
    yed_attrs  *ait;

    frame = event->frame;
    if (frame == NULL) { return; }

    buff = frame->buffer;
    if (buff == NULL) { return; }

    if (auto t = term_for_buffer(buff)) {
        event->row_base_attr = ZERO_ATTR;
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

static void term_new_cmd(int n_args, char **args) {
    Term *t = state->new_term();
    YEXE("buffer", t->buffer->name);
    term_mode = 1;
}

static void toggle_term_mode_cmd(int n_args, char **args) {
    toggle_term_mode();
}

static void unload(yed_plugin *self) {

}


static yed_event_handler draw_handler;
static yed_event_handler key_handler;
static yed_event_handler line_handler;
static yed_event_handler row_handler;
static yed_event_handler fresize_handler;
static yed_event_handler tresize_handler;
static yed_event_handler del_handler;
static yed_event_handler pre_buff_set_handler;
static yed_event_handler post_buff_set_handler;
static yed_event_handler sig_handler;

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    char *state_addr_str;
    char  addr_buff[64];

    YED_PLUG_VERSION_CHECK();

    if ((state_addr_str = yed_get_var(STATE_ADDR_VAR_NAME))) {
        sscanf(state_addr_str, "%p", (void**)&state);
    } else {
        state = new State;
        snprintf(addr_buff, sizeof(addr_buff), "%p", (void*)state);
        yed_set_var(STATE_ADDR_VAR_NAME, addr_buff);
    }

    if (!yed_get_var("terminal-shell")) {
        yed_set_var("terminal-shell", (char*)get_shell());
    }

    draw_handler.kind = EVENT_PRE_DRAW_EVERYTHING;
    draw_handler.fn   = update;
    yed_plugin_add_event_handler(self, draw_handler);

    key_handler.kind = EVENT_KEY_PRESSED;
    key_handler.fn   = key;
    yed_plugin_add_event_handler(self, key_handler);

    line_handler.kind = EVENT_LINE_PRE_DRAW;
    line_handler.fn   = line;
    yed_plugin_add_event_handler(self, line_handler);

    row_handler.kind = EVENT_ROW_PRE_CLEAR;
    row_handler.fn   = row;
    yed_plugin_add_event_handler(self, row_handler);

    fresize_handler.kind = EVENT_FRAME_POST_RESIZE;
    fresize_handler.fn   = fit;
    yed_plugin_add_event_handler(self, fresize_handler);

    tresize_handler.kind = EVENT_TERMINAL_RESIZED;
    tresize_handler.fn   = fit;
    yed_plugin_add_event_handler(self, tresize_handler);

    del_handler.kind = EVENT_FRAME_POST_DELETE;
    del_handler.fn   = fit;
    yed_plugin_add_event_handler(self, del_handler);

    pre_buff_set_handler.kind = EVENT_FRAME_PRE_SET_BUFFER;
    pre_buff_set_handler.fn   = fit;
    yed_plugin_add_event_handler(self, pre_buff_set_handler);

    post_buff_set_handler.kind = EVENT_FRAME_POST_SET_BUFFER;
    post_buff_set_handler.fn   = fit;
    yed_plugin_add_event_handler(self, post_buff_set_handler);

    sig_handler.kind = EVENT_SIGNAL_RECEIVED;
    sig_handler.fn   = sig;
    yed_plugin_add_event_handler(self, sig_handler);

    yed_plugin_set_command(self, "term-new", term_new_cmd);
    yed_plugin_set_command(self, "toggle-term-mode", toggle_term_mode_cmd);

    yed_plugin_set_unload_fn(self, unload);

    return 0;
}
