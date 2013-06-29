#include "thread.h"
#include "led-matrix.h"

#include <assert.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <vector>
#include <stdlib.h>
#include <algorithm>
#include <boost/asio.hpp>
#include <iostream>
#include "SDL/SDL.h"
#include "SDL/SDL_image.h"

using std::min;
using std::max;
using boost::asio::ip::tcp;

// Base-class for a Thread that does something with a matrix.
class RGBMatrixManipulator : public Thread {
public:
    RGBMatrixManipulator(RGBMatrix *m) : running_(true), matrix_(m) {}
    virtual ~RGBMatrixManipulator() { running_ = false; }

    // Run() implementation needs to check running_ regularly.

protected:
    volatile bool running_;  // TODO: use mutex, but this is good enough for now.
    RGBMatrix *const matrix_;
};

// Pump pixels to screen. Needs to be high priority real-time because jitter
// here will make the PWM uneven.
class DisplayUpdater : public RGBMatrixManipulator {
public:
    DisplayUpdater(RGBMatrix *m) : RGBMatrixManipulator(m) {}

    void Run() {
        while (running_) {
            matrix_->UpdateScreen();
        }
    }
};

class PNGReceiver : public RGBMatrixManipulator {
    Uint32 getpixel(SDL_Surface *surface, int x, int y)
    {
        int bpp = surface->format->BytesPerPixel;
        /* Here p is the address to the pixel we want to retrieve */
        Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

        switch(bpp) {
        case 1:
            return *p;
            break;

        case 2:
            return *(Uint16 *)p;
            break;

        case 3:
            if(SDL_BYTEORDER == SDL_BIG_ENDIAN)
                return p[0] << 16 | p[1] << 8 | p[2];
            else
                return p[0] | p[1] << 8 | p[2] << 16;
            break;

        case 4:
            return *(Uint32 *)p;
            break;

        default:
            return 0;       /* shouldn't happen, but avoids warnings */
        }
    }

public:
    PNGReceiver(RGBMatrix *m) : RGBMatrixManipulator(m) {}
    void Run() {
        SDL_Init( SDL_INIT_EVERYTHING );

        const int width = matrix_->width();
        const int height = matrix_->height();

        boost::asio::io_service io_service;
        tcp::acceptor acceptor(io_service, tcp::endpoint(tcp::v4(), 9000));

        for (;;) {
            // Create socket
            tcp::socket socket(io_service);

            // Accept a connection on the socket
            //std::cout << "waiting for connection" << std::endl;
            acceptor.accept(socket);

            boost::system::error_code ec;

            for (;;) {
                // Client connected, read image length
                size_t len = 0;
                boost::asio::read(socket, boost::asio::buffer(&len, sizeof(len)), boost::asio::transfer_exactly(4), ec);

                if (ec) {
                    //std::cout << "Failed to read length: " << ec << std::endl;
                    return;
                }

                //std::cout << "length " << len << std::endl;

                // Allocate memory for the image
                //char *imgMem = new char[len];
                //void *buf = new char[len];
                char buf[len];

                // Read the image
                std::size_t n = boost::asio::read(socket, boost::asio::buffer(buf, len), boost::asio::transfer_exactly(len), ec);

                if (ec) {
                    std::cout << "Failed to read image: " << ec << std::endl;
                    return;
                }

                assert(n == len);

                //std::cout << "Read " << n << " image bytes" << std::endl;

                // Create an RWops from the raw buffer
                SDL_RWops *imgRw = SDL_RWFromMem(buf, len);

                if (!imgRw) {
                    std::cout << "Failed to read image buffer into RWops " << std::endl;
                    return;
                }

                //std::cout << "Created image RWops" << std::endl;

                // Load PNG image from the RWops
                SDL_Surface *imgSurf = IMG_Load_RW(imgRw, 1);

                for (int x = 0; x < width; x++) {
                    for (int y = 0; y < height; y++) {
                        Uint32 px = getpixel(imgSurf, x, y);
                        Uint8 r, g, b;
                        SDL_GetRGB(px, imgSurf->format, &r, &g, &b);
                        matrix_->SetPixel(x, y, r, g, b);
                    }
                }

                //std::cout << "Iteration complete" << std::endl;
            }
        }
    }
};

int main(int argc, char *argv[]) {
    GPIO io;

    if (!io.Init())
        return 1;

    RGBMatrix m(&io);

    RGBMatrixManipulator *image_gen = NULL;
    image_gen = new PNGReceiver(&m);

    RGBMatrixManipulator *updater = new DisplayUpdater(&m);
    updater->Start(10);  // high priority

    image_gen->Start();

    // Things are set up. Just wait for <RETURN> to be pressed.
    printf("Press <RETURN> to exit and reset LEDs\n");
    getchar();

    // Stopping threads and wait for them to join.
    delete image_gen;
    delete updater;

    // Final thing before exit: clear screen and update once, so that
    // we don't have random pixels burn
    m.ClearScreen();
    m.UpdateScreen();

    return 0;
}
