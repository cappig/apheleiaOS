#include <riscv/mmio_plugin.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <sys/types.h>
#include <unistd.h>

#include <deque>
#include <string>

namespace {

constexpr reg_t UART_REG_THR_RBR_DLL = 0x00;
constexpr reg_t UART_REG_IER_DLM = 0x01;
constexpr reg_t UART_REG_IIR_FCR = 0x02;
constexpr reg_t UART_REG_LCR = 0x03;
constexpr reg_t UART_REG_MCR = 0x04;
constexpr reg_t UART_REG_LSR = 0x05;
constexpr reg_t UART_REG_MSR = 0x06;
constexpr reg_t UART_REG_SCR = 0x07;

constexpr uint8_t UART_LCR_DLAB = 0x80;

constexpr uint8_t UART_LSR_DR = 0x01;
constexpr uint8_t UART_LSR_THRE = 0x20;
constexpr uint8_t UART_LSR_TEMT = 0x40;

class ns16550a_plugin_t {
public:
    explicit ns16550a_plugin_t(const std::string &args)
        : args_(args) {
        const bool use_tty = args_.find("tty") != std::string::npos;
        if (use_tty) {
            int tty_in = open("/dev/tty", O_RDONLY | O_CLOEXEC);
            int tty_out = open("/dev/tty", O_WRONLY | O_CLOEXEC);

            if (tty_in >= 0 && tty_out >= 0) {
                stdin_fd_ = tty_in;
                stdout_fd_ = tty_out;
                close_stdin_ = true;
                close_stdout_ = true;
            } else {
                if (tty_in >= 0) {
                    close(tty_in);
                }
                if (tty_out >= 0) {
                    close(tty_out);
                }

                stdin_fd_ = STDIN_FILENO;
                stdout_fd_ = STDOUT_FILENO;
            }
        } else {
            stdin_fd_ = STDIN_FILENO;
            stdout_fd_ = STDOUT_FILENO;
        }

        if (isatty(stdin_fd_) == 1 && tcgetattr(stdin_fd_, &stdin_termios_) == 0) {
            stdin_termios_saved_ = true;

            struct termios raw = stdin_termios_;
            raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);
            raw.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            (void)tcsetattr(stdin_fd_, TCSANOW, &raw);
        }

        stdin_flags_ = fcntl(stdin_fd_, F_GETFL, 0);
        if (stdin_flags_ >= 0) {
            (void)fcntl(stdin_fd_, F_SETFL, stdin_flags_ | O_NONBLOCK);
        }
    }

    ~ns16550a_plugin_t() {
        if (stdin_flags_ >= 0) {
            (void)fcntl(stdin_fd_, F_SETFL, stdin_flags_);
        }

        if (stdin_termios_saved_) {
            (void)tcsetattr(stdin_fd_, TCSANOW, &stdin_termios_);
        }

        if (close_stdin_) {
            close(stdin_fd_);
        }
        if (close_stdout_) {
            close(stdout_fd_);
        }
    }

    bool load(reg_t addr, size_t len, uint8_t *bytes) {
        if (!bytes || len == 0) {
            return false;
        }

        for (size_t i = 0; i < len; i++) {
            if (!load8(addr + i, &bytes[i])) {
                return false;
            }
        }

        return true;
    }

    bool store(reg_t addr, size_t len, const uint8_t *bytes) {
        if (!bytes || len == 0) {
            return false;
        }

        for (size_t i = 0; i < len; i++) {
            if (!store8(addr + i, bytes[i])) {
                return false;
            }
        }

        return true;
    }

private:
    bool load8(reg_t addr, uint8_t *value) {
        if (!value) {
            return false;
        }

        pump_stdin();

        const reg_t reg = addr & 0x7;
        switch (reg) {
        case UART_REG_THR_RBR_DLL:
            if (lcr_ & UART_LCR_DLAB) {
                *value = dll_;
                return true;
            }

            if (rx_fifo_.empty()) {
                *value = 0;
                return true;
            }

            *value = rx_fifo_.front();
            rx_fifo_.pop_front();
            return true;

        case UART_REG_IER_DLM:
            *value = (lcr_ & UART_LCR_DLAB) ? dlm_ : ier_;
            return true;

        case UART_REG_IIR_FCR:
            *value = rx_fifo_.empty() ? 0x01 : 0x04;
            return true;

        case UART_REG_LCR:
            *value = lcr_;
            return true;

        case UART_REG_MCR:
            *value = mcr_;
            return true;

        case UART_REG_LSR: {
            uint8_t lsr = UART_LSR_THRE | UART_LSR_TEMT;
            if (!rx_fifo_.empty()) {
                lsr |= UART_LSR_DR;
            }
            *value = lsr;
            return true;
        }

        case UART_REG_MSR:
            *value = 0xB0;
            return true;

        case UART_REG_SCR:
            *value = scr_;
            return true;

        default:
            *value = 0;
            return true;
        }
    }

    bool store8(reg_t addr, uint8_t value) {
        const reg_t reg = addr & 0x7;
        switch (reg) {
        case UART_REG_THR_RBR_DLL:
            if (lcr_ & UART_LCR_DLAB) {
                dll_ = value;
                return true;
            }

            return write_stdout(value);

        case UART_REG_IER_DLM:
            if (lcr_ & UART_LCR_DLAB) {
                dlm_ = value;
            } else {
                ier_ = value;
            }
            return true;

        case UART_REG_IIR_FCR:
            return true;

        case UART_REG_LCR:
            lcr_ = value;
            return true;

        case UART_REG_MCR:
            mcr_ = value;
            return true;

        case UART_REG_SCR:
            scr_ = value;
            return true;

        default:
            return true;
        }
    }

    bool write_stdout(uint8_t ch) {
        ssize_t wrote = write(stdout_fd_, &ch, 1);
        if (wrote == 1) {
            return true;
        }

        if (wrote < 0 && (errno == EINTR || errno == EAGAIN)) {
            return true;
        }

        return false;
    }

    void pump_stdin() {
        if (stdin_fd_ < 0) {
            return;
        }

        uint8_t buf[256];
        while (true) {
            ssize_t n = read(stdin_fd_, buf, sizeof(buf));
            if (n > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    rx_fifo_.push_back(buf[i]);
                }
                continue;
            }

            if (n == 0) {
                return;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }

            return;
        }
    }

    std::string args_;
    std::deque<uint8_t> rx_fifo_;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stdin_flags_ = -1;
    bool close_stdin_ = false;
    bool close_stdout_ = false;
    bool stdin_termios_saved_ = false;
    struct termios stdin_termios_ = {};

    uint8_t ier_ = 0;
    uint8_t lcr_ = 0;
    uint8_t mcr_ = 0;
    uint8_t scr_ = 0;
    uint8_t dll_ = 0;
    uint8_t dlm_ = 0;
};

mmio_plugin_registration_t<ns16550a_plugin_t> reg_ns16550a_plugin("ns16550a");

} // namespace