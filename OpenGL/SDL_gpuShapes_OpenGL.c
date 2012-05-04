// Hacks to fix compile errors due to polluted namespace
#ifdef _WIN32
#define _WINUSER_H
#define _WINGDI_H
#endif

#include "SDL_gpu_OpenGL_internal.h"
#include "SDL_gpuShapes_OpenGL_internal.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265f
#endif

#ifndef DEGPERRAD 
#define DEGPERRAD 57.2957795f
#endif

#ifndef RADPERDEG 
#define RADPERDEG 0.0174532925f
#endif

static void Circle(GPU_ShapeRenderer* renderer, GPU_Target* target, float x, float y, float radius, SDL_Color color);




#define BEGIN \
	if(target == NULL) \
		return; \
	if(renderer->renderer != target->renderer) \
		return; \
	float z = ((RendererData_OpenGL*)renderer->data)->z;	\
	 \
	/* Bind the FBO */ \
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, ((TargetData_OpenGL*)target->data)->handle); \
	glPushAttrib(GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT); \
	if(target->useClip) \
	{ \
		glEnable(GL_SCISSOR_TEST); \
		int y = (renderer->renderer->display == target? renderer->renderer->display->h - (target->clipRect.y + target->clipRect.h) : target->clipRect.y); \
		float xFactor = ((float)SDL_GetVideoSurface()->w)/renderer->renderer->display->w; \
		float yFactor = ((float)SDL_GetVideoSurface()->h)/renderer->renderer->display->h; \
		glScissor(target->clipRect.x * xFactor, y * yFactor, target->clipRect.w * xFactor, target->clipRect.h * yFactor); \
	} \
	 \
	glDisable( GL_TEXTURE_2D );

#define END \
	if(target->useClip) \
	{ \
		glDisable(GL_SCISSOR_TEST); \
	} \
	glPopAttrib(); \
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0); \
	glEnable( GL_TEXTURE_2D );

	
#define INVERT_Y(y) \
	if(renderer->renderer->display != target) \
		(y) = renderer->renderer->display->h - (y);

static float SetThickness(GPU_ShapeRenderer* renderer, float thickness)
{
	float old;
	glGetFloatv(GL_LINE_WIDTH, &old);
	glLineWidth(thickness);
	return old;
}

static float GetThickness(GPU_ShapeRenderer* renderer)
{
	float old;
	glGetFloatv(GL_LINE_WIDTH, &old);
	return old;
}

static void Pixel(GPU_ShapeRenderer* renderer, GPU_Target* target, float x, float y, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_POINTS);
	glVertex3f(x, y, z);
	glEnd();
	
	END;
}

static void Line(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_LINES);
	glVertex3f(x1, y1, z);
	glVertex3f(x2, y2, z);
	glEnd();
	
	END;
}


static void Arc(GPU_ShapeRenderer* renderer, GPU_Target* target, float x, float y, float radius, float startAngle, float endAngle, SDL_Color color)
{
    float originalSA = startAngle;

    if(startAngle > endAngle)
    {
        float swapa = endAngle;
        endAngle = startAngle;
        startAngle = swapa;
    }
    if(startAngle == endAngle)
        return;
    
    // Big angle
    if(endAngle - startAngle >= 360)
    {
        Circle(renderer, target, x, y, radius, color);
        return;
    }
    
    // Shift together
    while(startAngle < 0 && endAngle < 0)
    {
        startAngle += 360;
        endAngle += 360;
    }
    while(startAngle > 360 && endAngle > 360)
    {
        startAngle -= 360;
        endAngle -= 360;
    }
    
    // Check if the angle to be drawn crosses 0
    Uint8 crossesZero = (startAngle < 0 && endAngle > 0) || (startAngle < 360 && endAngle > 360);
    
    // Push all values to 0 <= angle < 360
    while(startAngle >= 360)
        startAngle -= 360;
    while(endAngle >= 360)
        endAngle -= 360;
    while(startAngle < 0)
        startAngle += 360;
    while(endAngle < 0)
        endAngle += 360;
    
    if(endAngle == 0)
        endAngle = 360;
    else if(crossesZero)
    {
        Arc(renderer, target, x, y, radius, originalSA, 359.9f, color);
        startAngle = 0;
    }
	
	
	BEGIN;
	
	INVERT_Y(y);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	float t = startAngle;
	float dt = (1 - (endAngle - startAngle)/360) * 5;  // A segment every 5 degrees of a full circle
	float dx, dy;
	glBegin(GL_LINE_STRIP);
	dx = radius*cos(t*RADPERDEG);
	dy = radius*sin(t*RADPERDEG);
	glVertex3f(x+dx, y+dy, z);
	while(t < endAngle)
	{
		t += dt;
		dx = radius*cos(t*RADPERDEG);
		dy = radius*sin(t*RADPERDEG);
		glVertex3f(x+dx, y+dy, z);
	}
	glEnd();
	
	END;
}


static void ArcFilled(GPU_ShapeRenderer* renderer, GPU_Target* target, float x, float y, float radius, float startAngle, float endAngle, SDL_Color color)
{
    float originalSA = startAngle;

    if(startAngle > endAngle)
    {
        float swapa = endAngle;
        endAngle = startAngle;
        startAngle = swapa;
    }
    if(startAngle == endAngle)
        return;
    
    // Big angle
    if(endAngle - startAngle >= 360)
    {
        Circle(renderer, target, x, y, radius, color);
        return;
    }
    
    // Shift together
    while(startAngle < 0 && endAngle < 0)
    {
        startAngle += 360;
        endAngle += 360;
    }
    while(startAngle > 360 && endAngle > 360)
    {
        startAngle -= 360;
        endAngle -= 360;
    }
    
    // Check if the angle to be drawn crosses 0
    Uint8 crossesZero = (startAngle < 0 && endAngle > 0) || (startAngle < 360 && endAngle > 360);
    
    // Push all values to 0 <= angle < 360
    while(startAngle >= 360)
        startAngle -= 360;
    while(endAngle >= 360)
        endAngle -= 360;
    while(startAngle < 0)
        startAngle += 360;
    while(endAngle < 0)
        endAngle += 360;
    
    if(endAngle == 0)
        endAngle = 360;
    else if(crossesZero)
    {
        ArcFilled(renderer, target, x, y, radius, originalSA, 359.9f, color);
        startAngle = 0;
    }
	
	
	BEGIN;
	
	INVERT_Y(y);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	float t = startAngle;
	float dt = (1 - (endAngle - startAngle)/360) * 5;  // A segment every 5 degrees of a full circle
	float dx, dy;
	glBegin(GL_TRIANGLE_FAN);
	glVertex3f(x, y, z);
	dx = radius*cos(t*RADPERDEG);
	dy = radius*sin(t*RADPERDEG);
	while(t < endAngle)
	{
		glVertex3f(x+dx, y+dy, z);
		t += dt;
		dx = radius*cos(t*RADPERDEG);
		dy = radius*sin(t*RADPERDEG);
		glVertex3f(x+dx, y+dy, z);
	}
	glEnd();
	
	END;
}

static void Circle(GPU_ShapeRenderer* renderer, GPU_Target* target, float x, float y, float radius, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	float t = 0;
	float dt = 5;  // A segment every 5 degrees of a full circle
	float dx, dy;
	glBegin(GL_LINE_LOOP);
	dx = radius*cos(t*RADPERDEG);
	dy = radius*sin(t*RADPERDEG);
	glVertex3f(x+dx, y+dy, z);
	while(t < 360)
	{
		t += dt;
		dx = radius*cos(t*RADPERDEG);
		dy = radius*sin(t*RADPERDEG);
		glVertex3f(x+dx, y+dy, z);
	}
	glEnd();
	
	END;
}

static void CircleFilled(GPU_ShapeRenderer* renderer, GPU_Target* target, float x, float y, float radius, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	float t = 0;
	float dt = 5;  // A segment every 5 degrees of a full circle
	float dx, dy;
	glBegin(GL_POLYGON);
	dx = radius*cos(t*RADPERDEG);
	dy = radius*sin(t*RADPERDEG);
	glVertex3f(x+dx, y+dy, z);
	while(t < 360)
	{
		t += dt;
		dx = radius*cos(t*RADPERDEG);
		dy = radius*sin(t*RADPERDEG);
		glVertex3f(x+dx, y+dy, z);
	}
	glEnd();
	
	END;
}

static void Tri(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	INVERT_Y(y3);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_LINE_LOOP);
	glVertex3f(x1, y1, z);
	glVertex3f(x2, y2, z);
	glVertex3f(x3, y3, z);
	glEnd();
	
	END;
}

static void TriFilled(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	INVERT_Y(y3);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_TRIANGLE_STRIP);
	glVertex3f(x1, y1, z);
	glVertex3f(x2, y2, z);
	glVertex3f(x3, y3, z);
	glEnd();
	
	END;
}

static void Rect(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_LINE_LOOP);
	glVertex3f(x1, y1, z);
	glVertex3f(x1, y2, z);
	glVertex3f(x2, y2, z);
	glVertex3f(x2, y1, z);
	glEnd();
	
	END;
}

static void RectFilled(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, SDL_Color color)
{
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_TRIANGLE_STRIP);
	glVertex3f(x1, y1, z);
	glVertex3f(x1, y2, z);
	glVertex3f(x2, y1, z);
	glVertex3f(x2, y2, z);
	glEnd();
	
	END;
}

static void RectRound(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, float radius, SDL_Color color)
{
	if(y2 < y1)
	{
		float temp = y2;
		y2 = y1;
		y1 = temp;
	}
	if(x2 < x1)
	{
		float temp = x2;
		x2 = x1;
		x1 = temp;
	}
	
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_LINE_LOOP);
		glVertex3f(x1+radius,y1, z);
		glVertex3f(x2-radius,y1, z);
		for(float i=(float)M_PI*1.5f;i<M_PI*2;i+=0.1f)
			glVertex3f(x2-radius+cos(i)*radius,y1+radius+sin(i)*radius, z);
		glVertex3f(x2,y1+radius, z);
		glVertex3f(x2,y2-radius, z);
		for(float i=0;i<(float)M_PI*0.5f;i+=0.1f)
			glVertex3f(x2-radius+cos(i)*radius,y2-radius+sin(i)*radius, z);
		glVertex3f(x2-radius,y2, z);
		glVertex3f(x1+radius,y2, z);
		for(float i=(float)M_PI*0.5f;i<M_PI;i+=0.1f)
			glVertex3f(x1+radius+cos(i)*radius,y2-radius+sin(i)*radius, z);
		glVertex3f(x1,y2-radius, z);
		glVertex3f(x1,y1+radius, z);
		for(float i=(float)M_PI;i<M_PI*1.5f;i+=0.1f)
			glVertex3f(x1+radius+cos(i)*radius,y1+radius+sin(i)*radius, z);
	glEnd();
	
	END;
}

static void RectRoundFilled(GPU_ShapeRenderer* renderer, GPU_Target* target, float x1, float y1, float x2, float y2, float radius, SDL_Color color)
{
	if(y2 < y1)
	{
		float temp = y2;
		y2 = y1;
		y1 = temp;
	}
	if(x2 < x1)
	{
		float temp = x2;
		x2 = x1;
		x1 = temp;
	}
	
	BEGIN;
	
	INVERT_Y(y1);
	INVERT_Y(y2);
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	glBegin(GL_POLYGON);
		glVertex3f(x1+radius,y1, z);
		glVertex3f(x2-radius,y1, z);
		for(float i=(float)M_PI*1.5f;i<M_PI*2;i+=0.1f)
			glVertex3f(x2-radius+cos(i)*radius,y1+radius+sin(i)*radius, z);
		glVertex3f(x2,y1+radius, z);
		glVertex3f(x2,y2-radius, z);
		for(float i=0;i<(float)M_PI*0.5f;i+=0.1f)
			glVertex3f(x2-radius+cos(i)*radius,y2-radius+sin(i)*radius, z);
		glVertex3f(x2-radius,y2, z);
		glVertex3f(x1+radius,y2, z);
		for(float i=(float)M_PI*0.5f;i<M_PI;i+=0.1f)
			glVertex3f(x1+radius+cos(i)*radius,y2-radius+sin(i)*radius, z);
		glVertex3f(x1,y2-radius, z);
		glVertex3f(x1,y1+radius, z);
		for(float i=(float)M_PI;i<M_PI*1.5f;i+=0.1f)
			glVertex3f(x1+radius+cos(i)*radius,y1+radius+sin(i)*radius, z);
	glEnd();
	
	END;
}

static void Polygon(GPU_ShapeRenderer* renderer, GPU_Target* target, Uint16 n, float* vertices, SDL_Color color)
{
	BEGIN;
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	
	int i;
	glBegin(GL_LINE_LOOP);
	for(i = 0; i < 2*n; i+=2)
	{
		float y = vertices[i+1];
		
		INVERT_Y(y);
		
		glVertex3f(vertices[i], y, z);
	}
	glEnd();
	
	END;
}

static void PolygonFilled(GPU_ShapeRenderer* renderer, GPU_Target* target, Uint16 n, float* vertices, SDL_Color color)
{
	BEGIN;
	
	glColor4ub(color.r, color.g, color.b, color.unused);
	int i;
	glBegin(GL_POLYGON);
	for(i = 0; i < 2*n; i+=2)
	{
		float y = vertices[i+1];
		
		INVERT_Y(y);
		
		glVertex3f(vertices[i], y, z);
	}
	glEnd();
	
	END;
}










GPU_ShapeRenderer* GPU_CreateShapeRenderer_OpenGL(void)
{
	GPU_ShapeRenderer* renderer = (GPU_ShapeRenderer*)malloc(sizeof(GPU_ShapeRenderer));
	
	renderer->data = (ShapeRendererData_OpenGL*)malloc(sizeof(ShapeRendererData_OpenGL));
	
	renderer->SetThickness = &SetThickness;
	renderer->GetThickness = &GetThickness;
	renderer->Pixel = &Pixel;
	renderer->Line = &Line;
	renderer->Arc = &Arc;
	renderer->Circle = &Circle;
	renderer->CircleFilled = &CircleFilled;
	renderer->Tri = &Tri;
	renderer->TriFilled = &TriFilled;
	renderer->Rect = &Rect;
	renderer->RectFilled = &RectFilled;
	renderer->RectRound = &RectRound;
	renderer->RectRoundFilled = &RectRoundFilled;
	renderer->Polygon = &Polygon;
	renderer->PolygonFilled = &PolygonFilled;
	
	return renderer;
}

void GPU_FreeShapeRenderer_OpenGL(GPU_ShapeRenderer* renderer)
{
	if(renderer == NULL)
		return;
	
	free(renderer->data);
	free(renderer);
}
