#include <SDL3/SDL.h>
int main(int, char**) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_Window* window = SDL_CreateWindow("badlands", 1600, 900, SDL_WINDOW_RESIZABLE);
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) running = false;
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
