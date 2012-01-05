#include "SDL.h"
#include "SDL_gpu.h"

int main(int argc, char* argv[])
{
	GPU_Target* screen = GPU_Init(800, 600, 0);
	if(screen == NULL)
		return -1;
	
	printf("Renderer: %s\n", GPU_GetRendererString());
	
	GPU_Image* image = GPU_LoadImage("data/test.bmp");
	if(image == NULL)
		return -1;
	
	GPU_Image* image2 = GPU_LoadImage("data/test2.bmp");
	if(image2 == NULL)
		return -1;
	
	GPU_Target* target = GPU_LoadTarget(image);  // 502
	if(target == NULL)
		return -1;
	
	// Draw the second image onto the first
	GPU_BlitScale(image2, NULL, target, 0, 0, 0.3f, 0.3f);
	
	
	float frameTimeAvg = 1.0f;
	
	Uint8 done = 0;
	SDL_Event event;
	while(!done)
	{
		Uint32 frameStart = SDL_GetTicks();
		while(SDL_PollEvent(&event))
		{
			if(event.type == SDL_QUIT)
				done = 1;
			else if(event.type == SDL_KEYDOWN)
			{
				if(event.key.keysym.sym == SDLK_ESCAPE)
					done = 1;
			}
		}
		
		GPU_Clear(screen);
		
		GPU_Blit(image, NULL, screen, 50, 50);
		
		GPU_Flip();
		
		frameTimeAvg = (frameTimeAvg + (SDL_GetTicks() - frameStart)/1000.0f)/2;
	}
	
	printf("Average FPS: %.2f\n", 1/frameTimeAvg);
	
	GPU_FreeImage(image);
	GPU_Quit();
}


