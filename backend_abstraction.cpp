#define GLFW_INCLUDE_GLEXT
#include <GLFW/glfw3.h>

#include "glextloader.c"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <math.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "plug.h"
#include "plug_reload.cpp"

#define VORONOI_SEED_COUNT 40
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 800

#include "voronoi.cpp"

static GLuint program;
static GLuint vao;
static GLuint ssbo;
static GLFWwindow* window;

typedef struct Gpu_Seed {
	float pos[4];   // x, y, 0, 0
	float color[4]; // r, g, b, 1
} Gpu_Seed;

typedef struct Voronoi_Frag_Uniforms {
	int seed_count;
	int _pad0;
	int _pad1;
	int _pad2;
} Voronoi_Frag_Uniforms;

static Voronoi voronoi;

static char *slurp_file_into_malloced_cstr(const char *file_path)
{
	FILE *f = NULL;
	char *buffer = NULL;
	long size = 0;

	f = fopen(file_path, "r");
	if (f == NULL) goto fail;
	if (fseek(f, 0, SEEK_END) < 0) goto fail;

	size = ftell(f);
	if (size < 0) goto fail;

	buffer = (char *)malloc(size + 1);
	if (buffer == NULL) goto fail;

	if (fseek(f, 0, SEEK_SET) < 0) goto fail;

	fread(buffer, 1, size, f);
	if (ferror(f)) goto fail;

	buffer[size] = '\0';

	if (f) {
		fclose(f);
		errno = 0;
	}
	return buffer;
fail:
	if (f) {
		int saved_errno = errno;
		fclose(f);
		errno = saved_errno;
	}
	if (buffer) {
		free(buffer);
	}
	return NULL;
}

const char *shader_type_as_cstr(GLuint shader)
{
	switch (shader) {
	case GL_VERTEX_SHADER:
		return "GL_VERTEX_SHADER";
	case GL_FRAGMENT_SHADER:
		return "GL_FRAGMENT_SHADER";
	default:
		return "(Unknown)";
	}
}

static bool compile_shader_source(const GLchar *source, GLenum shader_type, GLuint *shader)
{
	*shader = glCreateShader(shader_type);
	glShaderSource(*shader, 1, &source, NULL);
	glCompileShader(*shader);

	GLint compiled = 0;
	glGetShaderiv(*shader, GL_COMPILE_STATUS, &compiled);

	if (!compiled) {
		GLchar message[1024];
		GLsizei message_size = 0;
		glGetShaderInfoLog(*shader, sizeof(message), &message_size, message);
		fprintf(stderr, "ERROR: could not compile %s\n", shader_type_as_cstr(shader_type));
		fprintf(stderr, "%.*s\n", message_size, message);
		return false;
	}

	return true;
}

static bool compile_shader_file(const char *file_path, GLenum shader_type, GLuint *shader)
{
	char *source = slurp_file_into_malloced_cstr(file_path);
	if (source == NULL) {
		fprintf(stderr, "ERROR: failed to read file `%s`: %s\n", file_path, strerror(errno));
		errno = 0;
		return false;
	}
	bool ok = compile_shader_source(source, shader_type, shader);
	if (!ok) {
		fprintf(stderr, "ERROR: failed to compile `%s` shader file\n", file_path);
	}
	free(source);
	return ok;
}

static bool link_program(GLuint vert_shader, GLuint frag_shader, GLuint *program)
{
	*program = glCreateProgram();

	glAttachShader(*program, vert_shader);
	glAttachShader(*program, frag_shader);
	glLinkProgram(*program);

	GLint linked = 0;
	glGetProgramiv(*program, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLsizei message_size = 0;
		GLchar message[1024];

		glGetProgramInfoLog(*program, sizeof(message), &message_size, message);
		fprintf(stderr, "Program Linking: %.*s\n", message_size, message);
		return false;
	}

	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);

	return true;
}

static bool load_shader_program(const char *vertex_file_path, const char *fragment_file_path, GLuint *program)
{
	GLuint vert = 0;
	if (!compile_shader_file(vertex_file_path, GL_VERTEX_SHADER, &vert)) {
		return false;
	}

	GLuint frag = 0;
	if (!compile_shader_file(fragment_file_path, GL_FRAGMENT_SHADER, &frag)) {
		return false;
	}

	if (!link_program(vert, frag, program)) {
		return false;
	}

	return true;
}

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
	Gpu_Seed *gpu_seeds = (Gpu_Seed *)malloc(sizeof(Gpu_Seed) * v->size);
	if (!gpu_seeds) {
		return;
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

	free(gpu_seeds);
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
