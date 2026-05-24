#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <math.h>

#include <SDL3/SDL.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include "plug.h"

#define VORONOI_SEED_COUNT 40
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 800

#include "voronoi.cpp"

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

static SDL_Window *window;
static SDL_GPUDevice *gpu_device;
static SDL_GPUGraphicsPipeline *gpu_pipeline;
static SDL_GPUTransferBuffer *gpu_transfer_buffer;
static SDL_GPUBuffer *gpu_buffer;

static const char *lib_plug_name = "./lib_plug.so";
static void *libplug;

static plug_init_t plug_init;
static plug_update_t plug_update;
static plug_pre_reload_t plug_pre_reload;
static plug_post_reload_t plug_post_reload;

void plug_reload(void)
{
	if (libplug)
		dlclose(libplug);

	for (int attempt = 0; attempt < 10; attempt++) {
		libplug = dlopen(lib_plug_name, RTLD_NOW | RTLD_GLOBAL);
		if (libplug)
			break;
		usleep(100000);
	}

	if (!libplug) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		exit(1);
	}

	plug_init = (plug_init_t)dlsym(libplug, "plug_init");
	if (!plug_init) {
		fprintf(stderr, "dlsym plug_init: %s\n", dlerror());
		exit(1);
	}

	plug_update = (plug_update_t)dlsym(libplug, "plug_update");
	if (!plug_update) {
		fprintf(stderr, "dlsym plug_update: %s\n", dlerror());
		exit(1);
	}

	plug_pre_reload = (plug_pre_reload_t)dlsym(libplug, "plug_pre_reload");
	if (!plug_pre_reload) {
		fprintf(stderr, "dlsym plug_pre_reload: %s\n", dlerror());
		exit(1);
	}

	plug_post_reload = (plug_post_reload_t)dlsym(libplug, "plug_post_reload");
	if (!plug_post_reload) {
		fprintf(stderr, "dlsym plug_post_reload: %s\n", dlerror());
		exit(1);
	}

	printf("reloading plug\n");
}

int plug_should_reload(time_t *last_mtime)
{
	struct stat st;

	if (stat(lib_plug_name, &st) != 0) {
		fprintf(stderr, "stat(%s) failed: %s\n", lib_plug_name, strerror(errno));
		exit(1);
	}

	if (st.st_mtime != *last_mtime) {
		*last_mtime = st.st_mtime;
		return 1;
	}

	return 0;
}

static SDL_GPUShader *load_shader_spirv(SDL_GPUDevice *gpu, const char *path, SDL_GPUShaderStage stage, Uint32 num_uniform_buffers, Uint32 num_storage_buffers)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "fopen(%s) failed\n", path);
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0) {
		fprintf(stderr, "shader file %s is empty\n", path);
		fclose(f);
		return NULL;
	}

	Uint8 *code = (Uint8 *)malloc(size);
	if (!code) {
		fclose(f);
		return NULL;
	}

	if (fread(code, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "failed to read shader file %s\n", path);
		fclose(f);
		free(code);
		return NULL;
	}

	fclose(f);

	SDL_GPUShaderCreateInfo info = {};
	info.code_size = (size_t)size;
	info.code = code;
	info.entrypoint = "main";
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage = stage;

	info.num_samplers = 0;
	info.num_storage_textures = 0;
	info.num_storage_buffers = num_storage_buffers;
	info.num_uniform_buffers = num_uniform_buffers;
	info.props = 0;

	SDL_GPUShader *shader = SDL_CreateGPUShader(gpu, &info);
	free(code);

	if (!shader) {
		fprintf(stderr, "SDL_CreateGPUShader(%s) failed: %s\n", path, SDL_GetError());
		return NULL;
	}

	return shader;
}

static void init_GPU_pipeline(SDL_GPUDevice *gpu, SDL_GPUGraphicsPipeline **pipeline, SDL_GPUTextureFormat format)
{
	SDL_GPUShader *vert = load_shader_spirv(gpu, "voronoi.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
	SDL_GPUShader *frag = load_shader_spirv(gpu, "voronoi.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

	if (!vert || !frag) {
		fprintf(stderr, "Failed to load voronoi shaders\n");
		exit(1);
	}

	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = format;
	color_target_desc.blend_state.enable_blend = false;
	color_target_desc.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

	SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {};
	pipeline_info.vertex_shader = vert;
	pipeline_info.fragment_shader = frag;
	pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

	pipeline_info.vertex_input_state.vertex_buffer_descriptions = NULL;
	pipeline_info.vertex_input_state.num_vertex_buffers = 0;
	pipeline_info.vertex_input_state.vertex_attributes = NULL;
	pipeline_info.vertex_input_state.num_vertex_attributes = 0;

	pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	pipeline_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

	pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

	pipeline_info.depth_stencil_state.enable_depth_test = false;
	pipeline_info.depth_stencil_state.enable_depth_write = false;
	pipeline_info.depth_stencil_state.enable_stencil_test = false;

	pipeline_info.target_info.color_target_descriptions = &color_target_desc;
	pipeline_info.target_info.num_color_targets = 1;
	pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	pipeline_info.target_info.has_depth_stencil_target = false;

	*pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &pipeline_info);
	if (!(*pipeline)) {
		fprintf(stderr, "SDL_CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_ReleaseGPUShader(gpu, vert);
	SDL_ReleaseGPUShader(gpu, frag);
}

static void init_GPU_buffer(SDL_GPUDevice *gpu, SDL_GPUTransferBuffer **transfer_buffer, SDL_GPUBuffer **buffer, uint32_t size)
{
	assert(size != 0);
	SDL_GPUBufferCreateInfo buffer_info = {};
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	buffer_info.size = sizeof(Gpu_Seed) * size;
	buffer_info.props = 0;

	*buffer = SDL_CreateGPUBuffer(gpu, &buffer_info);
	if (!(*buffer)) {
		fprintf(stderr, "SDL_CreateGPUBuffer failed: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(Gpu_Seed) * size;
	transfer_info.props = 0;

	*transfer_buffer = SDL_CreateGPUTransferBuffer(gpu, &transfer_info);

	if (!(*transfer_buffer)) {
		fprintf(stderr, "SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());
		exit(1);
	}
}

static void init_sdl(SDL_Window **window, SDL_GPUDevice **gpu, SDL_GPUGraphicsPipeline **pipeline, SDL_GPUTransferBuffer **transfer_buffer, SDL_GPUBuffer **buffer, SDL_GPUTextureFormat *format, size_t size)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		fprintf(stderr, "Error: SDL_Init(): %s\n", SDL_GetError());
		exit(1);
	}

	float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
	SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;

	*window = SDL_CreateWindow("voronoi", (int)(WINDOW_WIDTH * main_scale), (int)(WINDOW_HEIGHT * main_scale), window_flags);

	if ((*window) == NULL) {
		fprintf(stderr, "Error: SDL_CreateWindow(): %s\n", SDL_GetError());
		exit(1);
	}

	*gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, NULL);

	if ((*gpu) == NULL) {
		fprintf(stderr, "Error: SDL_CreateGPUDevice(): %s\n", SDL_GetError());
		exit(1);
	}

	if (!SDL_ClaimWindowForGPUDevice(*gpu, *window)) {
		fprintf(stderr, "Error: SDL_ClaimWindowForGPUDevice(): %s\n", SDL_GetError());
		exit(1);
	}

	SDL_SetGPUSwapchainParameters(*gpu, *window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

	SDL_SetWindowPosition(*window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	*format = SDL_GetGPUSwapchainTextureFormat(*gpu, *window);
	init_GPU_pipeline(*gpu, pipeline, *format);
	init_GPU_buffer(*gpu, transfer_buffer, buffer, size);
	SDL_ShowWindow(*window);
}

void backend_init(void)
{
	init_voronoi(&voronoi, WINDOW_WIDTH, WINDOW_HEIGHT, VORONOI_SEED_COUNT);

	SDL_GPUTextureFormat format = {};
	init_sdl(&window, &gpu_device, &gpu_pipeline, &gpu_transfer_buffer, &gpu_buffer, &format, voronoi.size);

	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGui::StyleColorsDark();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui_ImplSDL3_InitForSDLGPU(window);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = gpu_device;
	init_info.ColorTargetFormat = format;
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
	init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;

	ImGui_ImplSDLGPU3_Init(&init_info);
}

static void prepare_GPU_frame(Voronoi *v, SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *command_buffer, SDL_GPUTransferBuffer *transfer_buffer, SDL_GPUBuffer *buffer)
{
	Gpu_Seed *mapped = (Gpu_Seed *)SDL_MapGPUTransferBuffer(gpu, transfer_buffer, true);

	if (!mapped) {
		fprintf(stderr, "SDL_MapGPUTransferBuffer failed: %s\n", SDL_GetError());
		return;
	}

	for (size_t i = 0; i < v->size; i++) {
		Voronoi_Seed *s = &v->seeds[i];

		mapped[i].pos[0] = s->x;
		mapped[i].pos[1] = s->y;
		mapped[i].pos[2] = 0.0f;
		mapped[i].pos[3] = 0.0f;

		mapped[i].color[0] = (float)s->r / 255.0f;
		mapped[i].color[1] = (float)s->g / 255.0f;
		mapped[i].color[2] = (float)s->b / 255.0f;
		mapped[i].color[3] = 1.0f;
	}

	SDL_UnmapGPUTransferBuffer(gpu, transfer_buffer);

	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

	SDL_GPUTransferBufferLocation src = {};
	src.transfer_buffer = transfer_buffer;
	src.offset = 0;

	SDL_GPUBufferRegion dst = {};
	dst.buffer = buffer;
	dst.offset = 0;
	dst.size = sizeof(Gpu_Seed) * v->size;

	SDL_UploadToGPUBuffer(copy_pass, &src, &dst, true);

	SDL_EndGPUCopyPass(copy_pass);
}

void render(void)
{
	ImGui::Render();

	ImDrawData *draw_data = ImGui::GetDrawData();

	bool is_minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;

	SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(gpu_device);

	if (command_buffer == NULL) {
		fprintf(stderr, "SDL_AcquireGPUCommandBuffer failed: %s\n", SDL_GetError());
		return;
	}

	SDL_GPUTexture *swapchain_texture = NULL;
	Uint32 swapchain_w = 0;
	Uint32 swapchain_h = 0;

	if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, window, &swapchain_texture, &swapchain_w, &swapchain_h)) {
		fprintf(stderr, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s\n", SDL_GetError());
		SDL_CancelGPUCommandBuffer(command_buffer);
		return;
	}

	if (swapchain_texture != NULL && !is_minimized) {
		ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

		float dt = ImGui::GetIO().DeltaTime;
		update_voronoi_background(&voronoi, dt, (int)swapchain_w, (int)swapchain_h);
		prepare_GPU_frame(&voronoi, gpu_device, command_buffer, gpu_transfer_buffer, gpu_buffer);

		SDL_GPUColorTargetInfo target_info = {};
		target_info.texture = swapchain_texture;
		target_info.clear_color = SDL_FColor {
			20.0f / 255.0f,
			25.0f / 255.0f,
			45.0f / 255.0f,
			1.0f
		};
		target_info.load_op = SDL_GPU_LOADOP_CLEAR;
		target_info.store_op = SDL_GPU_STOREOP_STORE;
		target_info.mip_level = 0;
		target_info.layer_or_depth_plane = 0;
		target_info.cycle = false;

		SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, NULL);
		SDL_BindGPUGraphicsPipeline(render_pass, gpu_pipeline);
		SDL_BindGPUFragmentStorageBuffers(render_pass, 0, &gpu_buffer, 1);
		Voronoi_Frag_Uniforms u = {};
		u.seed_count = (int)voronoi.size;
		SDL_PushGPUFragmentUniformData(command_buffer, 0, &u, sizeof(u));
		SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);

		ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
		SDL_EndGPURenderPass(render_pass);
	}

	SDL_SubmitGPUCommandBuffer(command_buffer);
}

void backend_exit(void)
{
	SDL_WaitForGPUIdle(gpu_device);

	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();

	ImPlot::DestroyContext();
	ImGui::DestroyContext();

	if (gpu_pipeline) {
		SDL_ReleaseGPUGraphicsPipeline(gpu_device, gpu_pipeline);
		gpu_pipeline = NULL;
	}
	if (gpu_transfer_buffer) {
		SDL_ReleaseGPUTransferBuffer(gpu_device, gpu_transfer_buffer);
		gpu_transfer_buffer = NULL;
	}
	if (gpu_buffer) {
		SDL_ReleaseGPUBuffer(gpu_device, gpu_buffer);
		gpu_buffer = NULL;
	}

	SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
	SDL_DestroyGPUDevice(gpu_device);
	SDL_DestroyWindow(window);

	SDL_Quit();
	destroy_voronoi(&voronoi);
}

void new_frame(void)
{
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

void check_for_exit(bool *done)
{
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		ImGui_ImplSDL3_ProcessEvent(&event);

		if (event.type == SDL_EVENT_QUIT) {
			*done = true;
		}

		if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
		    event.window.windowID == SDL_GetWindowID(window)) {
			*done = true;
		}
	}
}
