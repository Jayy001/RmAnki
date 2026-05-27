#include "qtfb-client.h"

#include <iostream>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

qtfb::ClientConnection::ClientConnection(
    qtfb::FBKey framebufferID,
    uint8_t shmType,
    std::optional<std::tuple<uint16_t, uint16_t>> customResolution,
    bool nonBlocking)
{
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock == -1) {
        std::cerr << "QTFB: Failed to create socket: " << strerror(errno) << std::endl;
        exit(-1);
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        std::cerr << "QTFB: Failed to connect to " << SOCKET_PATH << ": " << strerror(errno) << std::endl;
        exit(-2);
    }

    qtfb::ClientMessage initMessage;
    if (auto resolution = customResolution) {
        _width  = std::get<0>(*resolution);
        _height = std::get<1>(*resolution);
        initMessage = {
            .type = MESSAGE_CUSTOM_INITIALIZE,
            .customInit = {
                .framebufferKey  = framebufferID,
                .framebufferType = shmType,
                .width           = _width,
                .height          = _height,
            },
        };
    } else {
        initMessage = {
            .type = MESSAGE_INITIALIZE,
            .init = {
                .framebufferKey  = framebufferID,
                .framebufferType = shmType,
            },
        };
        switch (shmType) {
        case FBFMT_RM2FB:
            _width = RM2_WIDTH; _height = RM2_HEIGHT; break;
        case FBFMT_RMPP_RGB565:
        case FBFMT_RMPP_RGB888:
        case FBFMT_RMPP_RGBA8888:
            _width = RMPP_WIDTH; _height = RMPP_HEIGHT; break;
        case FBFMT_RMPPM_RGB565:
        case FBFMT_RMPPM_RGB888:
        case FBFMT_RMPPM_RGBA8888:
            _width = RMPPM_WIDTH; _height = RMPPM_HEIGHT; break;
        }
    }

    if (send(sock, &initMessage, sizeof(initMessage), 0) == -1) {
        std::cerr << "QTFB: Failed to send init message" << std::endl;
        exit(-3);
    }

    qtfb::ServerMessage initConfirm;
    if (recv(sock, &initConfirm, sizeof(initConfirm), 0) < 1) {
        std::cerr << "QTFB: Failed to receive init confirmation" << std::endl;
        exit(-4);
    }

    if (nonBlocking) {
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1) {
            std::cerr << "QTFB: fcntl F_GETFL failed" << std::endl;
            exit(-7);
        }
        if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
            std::cerr << "QTFB: fcntl F_SETFL failed" << std::endl;
            exit(-8);
        }
    }

    FORMAT_SHM(shmName, initConfirm.init.shmKeyDefined);
    int sfd = shm_open(shmName, O_RDWR, 0);
    if (sfd == -1) {
        std::cerr << "QTFB: shm_open failed: " << strerror(errno) << std::endl;
        exit(-5);
    }

    unsigned char *memory = (unsigned char *)mmap(
        NULL, initConfirm.init.shmSize,
        PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    if (memory == MAP_FAILED) {
        std::cerr << "QTFB: mmap failed: " << strerror(errno) << std::endl;
        exit(-6);
    }

    this->fd      = sock;
    this->shmFD   = sfd;
    this->shm     = memory;
    this->shmSize = initConfirm.init.shmSize;
}

qtfb::ClientConnection::~ClientConnection()
{
    if (shm && shmSize)
        munmap(shm, shmSize);
    if (shmFD >= 0)
        close(shmFD);

    qtfb::ClientMessage terminateMessage = { .type = MESSAGE_TERMINATE };
    _send(terminateMessage);

    if (fd >= 0)
        close(fd);
}

unsigned short qtfb::ClientConnection::width()  const { return _width;  }
unsigned short qtfb::ClientConnection::height() const { return _height; }
int qtfb::ClientConnection::getRefreshMode()    const { return _refreshMode; }

void qtfb::ClientConnection::_send(const struct ClientMessage &msg)
{
    if (fd >= 0)
        send(fd, &msg, sizeof(msg), 0);
}

void qtfb::ClientConnection::sendCompleteUpdate()
{
    _send({
        .type   = MESSAGE_UPDATE,
        .update = { .type = UPDATE_ALL },
    });
}

void qtfb::ClientConnection::sendPartialUpdate(int x, int y, int w, int h)
{
    _send({
        .type   = MESSAGE_UPDATE,
        .update = { .type = UPDATE_PARTIAL, .x = x, .y = y, .w = w, .h = h },
    });
}

bool qtfb::ClientConnection::pollServerPacket(struct ServerMessage &message)
{
    return recv(fd, &message, sizeof(message), 0) >= 1;
}

void qtfb::ClientConnection::setRefreshMode(int refreshMode)
{
    _refreshMode = refreshMode;
    _send({
        .type        = MESSAGE_SET_REFRESH_MODE,
        .refreshMode = refreshMode,
    });
}

void qtfb::ClientConnection::requestFullRefresh()
{
    _send({ .type = MESSAGE_REQUEST_FULL_REFRESH });
}

qtfb::FBKey qtfb::getIDFromAppload()
{
    const char *key = getenv("QTFB_KEY");
    if (!key) {
        std::cerr << "QTFB: QTFB_KEY not set — not launched via AppLoad!" << std::endl;
        abort();
    }
    return (qtfb::FBKey)strtol(key, nullptr, 10);
}
