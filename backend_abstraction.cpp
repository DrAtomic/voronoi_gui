#define GLFW_INCLUDE_GLEXT
#include <GLFW/glfw3.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "compile_shaders.cpp"
#include "plug_reload.cpp"
#include "voronoi.cpp"

#define VORONOI_SEED_COUNT 40
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 800

static GLuint program;
static GLuint vao;
static GLuint ssbo;
static GLFWwindow* window;

typedef struct Gpu_Seed {
	float pos[4];   // x, y, 0, 0
	float color[4]; // r, g, b, 1
} Gpu_Seed;

static Voronoi voronoi;

static void init_voronoi_gl(size_t seed_capacity)
{
	const char *vertex_file_path = "voronoi.vert";
	const char *fragment_file_path = "voronoi.frag";

	if (!load_shader_program(vertex_file_path, fragment_file_path, &program)) {
		exit(1);
	}

	glGenVertexArrays(1, &vao);

	glGenBuffers(1, &ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(Gpu_Seed) * seed_capacity, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void backend_init(void)
{
	glfwSetErrorCallback(glfw_error_callback);

	if (!glfwInit()) {
		exit(1);
	}

	const char* glsl_version = "#version 430";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
	window = glfwCreateWindow((int)(WINDOW_WIDTH * main_scale), (int)(WINDOW_HEIGHT * main_scale), "voronoi",  NULL, NULL);
	if (window == NULL) {
		exit(1);
	}

	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	load_gl_extensions();

	bool failed_to_find_function = false;
	failed_to_find_function |= glCreateShader == NULL;
	failed_to_find_function |= glGenBuffers == NULL;
	failed_to_find_function |= glBindBufferBase == NULL;
	failed_to_find_function |= glDeleteVertexArrays == NULL;
	failed_to_find_function |= glDeleteBuffers == NULL;
	failed_to_find_function |= glBufferSubData == NULL;

	if (failed_to_find_function) {
		fprintf(stderr, "required OpenGL functions were not loaded\n");
		exit(1);
	}

	init_voronoi(&voronoi, WINDOW_WIDTH,  WINDOW_HEIGHT, VORONOI_SEED_COUNT);

	init_voronoi_gl(VORONOI_SEED_COUNT);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGui::StyleColorsDark();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(main_scale);
	style.FontScaleDpi = main_scale;

	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);
}

static void upload_voronoi_seeds(Voronoi *v)
{
	static Gpu_Seed *gpu_seeds;
	if (gpu_seeds == NULL) {
		// its lifetime is whole program. don't care about freeing
		gpu_seeds = (Gpu_Seed *)malloc(sizeof(Gpu_Seed) * v->size);
		if (gpu_seeds == NULL) {
			fprintf(stderr, "failed to allocate seeds\n");
			exit(1);
		}
	}

	for (size_t i = 0; i < v->size; i++) {
		Voronoi_Seed *s = &v->seeds[i];

		gpu_seeds[i].pos[0] = s->x;
		gpu_seeds[i].pos[1] = s->y;
		gpu_seeds[i].pos[2] = 0.0f;
		gpu_seeds[i].pos[3] = 0.0f;

		gpu_seeds[i].color[0] = (float)s->r / 255.0f;
		gpu_seeds[i].color[1] = (float)s->g / 255.0f;
		gpu_seeds[i].color[2] = (float)s->b / 255.0f;
		gpu_seeds[i].color[3] = 1.0f;
	}

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(Gpu_Seed) * v->size, gpu_seeds);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

static void draw_voronoi_gl(Voronoi *v, int framebuffer_w, int framebuffer_h)
{
	upload_voronoi_seeds(v);

	glUseProgram(program);

	GLint seed_count_loc = glGetUniformLocation(program, "seed_count");
	glUniform1i(seed_count_loc, (int)v->size);

	GLint resolution_loc = glGetUniformLocation(program, "resolution");
	if (resolution_loc >= 0) {
		glUniform2f(resolution_loc, (float)framebuffer_w, (float)framebuffer_h);
	}

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
	glUseProgram(0);
}

void render(void)
{
	ImGui::Render();

	int display_w, display_h;
	glfwGetFramebufferSize(window, &display_w, &display_h);
	float dt = ImGui::GetIO().DeltaTime;
	update_voronoi_background(&voronoi, dt, display_w, display_h);

	glViewport(0, 0, display_w, display_h);
	draw_voronoi_gl(&voronoi, display_w, display_h);

	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glfwSwapBuffers(window);
}

void backend_exit(void)
{
	glDeleteBuffers(1, &ssbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(program);

	destroy_voronoi(&voronoi);

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();

	ImPlot::DestroyContext();
	ImGui::DestroyContext();

	glfwDestroyWindow(window);
	glfwTerminate();
}

void new_frame(void)
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
}

void check_for_exit(bool *done)
{
	*done = glfwWindowShouldClose(window);
	glfwPollEvents();
	if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
		ImGui_ImplGlfw_Sleep(10);
	}
}
