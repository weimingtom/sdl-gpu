#include "SDL.h"
#include "SDL_gpu.h"
#include <math.h>

int main(int argc, char* argv[])
{
	GPU_Target* screen = GPU_Init(800, 600, 0);
	if(screen == NULL)
		return -1;
	
	printf("Renderer: %s\n", GPU_GetRendererString());
	
	GPU_Image* image = GPU_LoadImage("data/test.bmp");
	if(image == NULL)
		return -1;
	
	SDL_Color transColor = {255, 255, 255};
	GPU_MakeColorTransparent(image, transColor);
	
	float x = 0, y = 0;
	float velx = 50.0f, vely = 70.0f;
	
	float dt = 0.010f;
	
	Uint32 startTime = SDL_GetTicks();
	long frameCount = 0;
	
	Uint8 done = 0;
	SDL_Event event;
	while(!done)
	{
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
		
		x += velx*dt;
		y += vely*dt;
		if(x < 0)
		{
			x = 0;
			velx = -velx;
		}
		else if(x + image->w > screen->w)
		{
			x = screen->w - image->w;
			velx = -velx;
		}
		
		if(y < 0)
		{
			y = 0;
			vely = -vely;
		}
		else if(y + image->h > screen->h)
		{
			y = screen->h - image->h;
			vely = -vely;
		}
		
		GPU_Clear(screen);
		
		GPU_Blit(image, NULL, screen, 50, 50);
		GPU_Blit(image, NULL, screen, 150, 50);
		GPU_Blit(image, NULL, screen, 350, 250);
		
		GPU_SetBlending(1);
		
		GPU_SetRGBA(255, 255, 255, 127.5f + 127.5f*sin(SDL_GetTicks()/1000.0f));
		GPU_Blit(image, NULL, screen, x, y);
		GPU_SetBlending(0);
		
		GPU_Flip();
		
		frameCount++;
		if(frameCount%500 == 0)
			printf("Average FPS: %.2f\n", 1000.0f*frameCount/(SDL_GetTicks() - startTime));
	}
	
	printf("Average FPS: %.2f\n", 1000.0f*frameCount/(SDL_GetTicks() - startTime));
	
	GPU_FreeImage(image);
	GPU_Quit();
}


