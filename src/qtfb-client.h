#pragma once
#include "common.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>

#include <optional>
#include <tuple>

namespace qtfb {
    class ClientConnection {
    public:
        ClientConnection(FBKey framebufferID, uint8_t shmType,
                         std::optional<std::tuple<uint16_t, uint16_t>> customResolution = {},
                         bool nonBlocking = true);
        ~ClientConnection();

        void sendCompleteUpdate();
        void sendPartialUpdate(int x, int y, int w, int h);

        unsigned char *shm = nullptr;
        int shmFD = -1;
        size_t shmSize = 0;

        bool pollServerPacket(struct ServerMessage &message);
        unsigned short width() const;
        unsigned short height() const;

        void setRefreshMode(int refreshMode);
        int getRefreshMode() const;
        void requestFullRefresh();

        int socketFd() const { return fd; }

    private:
        int fd = -1;                  // unix socket fd
        unsigned short _width  = 0;
        unsigned short _height = 0;
        int _refreshMode = REFRESH_MODE_ANIMATE;
        void _send(const struct ClientMessage &message);
    };

    FBKey getIDFromAppload();
}
