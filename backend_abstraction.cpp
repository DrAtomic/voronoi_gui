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

static SDL_Window *window;
static SDL_GPUDevice *gpu_device;
static ImGuiIO io;

static const char *lib_plug_name = "./lib_plug.so";
static void *libplug;

static plug_init_t plug_init;
static plug_update_t plug_update;
static plug_pre_reload_t plug_pre_reload;
static plug_post_reload_t plug_post_reload;

static void init_voronoi_pipeline(void);
static void init_voronoi_seed_buffer(void);

static void plug_reload(void)
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

static int plug_should_reload(time_t *last_mtime)
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

static void init_sdl(void)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		fprintf(stderr, "Error: SDL_Init(): %s\n", SDL_GetError());
		exit(1);
	}

	float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

	SDL_WindowFlags window_flags =
		SDL_WINDOW_RESIZABLE |
		SDL_WINDOW_HIDDEN |
		SDL_WINDOW_HIGH_PIXEL_DENSITY;

	window = SDL_CreateWindow("voronoi",
	                          (int)(1280 * main_scale),
	                          (int)(800 * main_scale),
	                          window_flags);

	if (window == NULL) {
		fprintf(stderr, "Error: SDL_CreateWindow(): %s\n", SDL_GetError());
		exit(1);
	}

	gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,
	                                 true,
	                                 NULL);

	if (gpu_device == NULL) {
		fprintf(stderr, "Error: SDL_CreateGPUDevice(): %s\n", SDL_GetError());
		exit(1);
	}

	if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
		fprintf(stderr, "Error: SDL_ClaimWindowForGPUDevice(): %s\n", SDL_GetError());
		exit(1);
	}

	SDL_SetGPUSwapchainParameters(gpu_device,
	                              window,
	                              SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
	                              SDL_GPU_PRESENTMODE_VSYNC);

	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_ShowWindow(window);
}

static void backend_init(void)
{
	init_sdl();

	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGui::StyleColorsDark();

	io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui_ImplSDL3_InitForSDLGPU(window);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = gpu_device;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
	init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;

	ImGui_ImplSDLGPU3_Init(&init_info);

	init_voronoi_pipeline();
	init_voronoi_seed_buffer();
}

static SDL_GPUGraphicsPipeline *voronoi_pipeline = NULL;

#define VORONOI_MAX_SEEDS 1000

struct VoronoiSeed {
	float x;
	float y;
	float vx;
	float vy;
	Uint8 r;
	Uint8 g;
	Uint8 b;
};

struct VoronoiBackground {
	bool initialized;
	VoronoiSeed seeds[VORONOI_MAX_SEEDS];
};

struct VoronoiGpuSeed {
	float pos[4];   // x, y, 0, 0
	float color[4]; // r, g, b, 1
};

struct VoronoiFragUniforms {
	float screen_time_count[4]; // width, height, time, active_count
};

static VoronoiBackground voronoi;
static VoronoiGpuSeed voronoi_gpu_seeds[VORONOI_MAX_SEEDS];

static SDL_GPUBuffer *voronoi_seed_buffer = NULL;
static SDL_GPUTransferBuffer *voronoi_seed_transfer_buffer = NULL;

static int voronoi_active_seed_count = 50;
static float voronoi_time = 0.0f;

static float rand_float01(void)
{
	return (float)rand() / (float)RAND_MAX;
}

static float rand_float_range(float min, float max)
{
	return min + (max - min) * rand_float01();
}

static void init_voronoi_background(int w, int h)
{
	voronoi.initialized = true;

	for (int i = 0; i < VORONOI_MAX_SEEDS; i++) {
		VoronoiSeed *s = &voronoi.seeds[i];

		s->x = rand_float_range(0.0f, (float)w);
		s->y = rand_float_range(0.0f, (float)h);

		float angle = rand_float_range(0.0f, 6.28318530718f);
		float speed = rand_float_range(1.0f, 10.0f);

		s->vx = cosf(angle) * speed;
		s->vy = sinf(angle) * speed;

		float r, g, b;
		ImGui::ColorConvertHSVtoRGB(0.58, 0.55, rand_float_range(0.40, 0.55), r, g, b);

		s->r = (Uint8)(r * 255.0f);
		s->g = (Uint8)(g * 255.0f);
		s->b = (Uint8)(b * 255.0f);
	}
}

static void update_voronoi_background(float dt, int w, int h)
{
	if (dt > 0.033f) {
		dt = 0.033f;
	}

	for (int i = 0; i < VORONOI_MAX_SEEDS; i++) {
		VoronoiSeed *s = &voronoi.seeds[i];

		s->x += s->vx * dt;
		s->y += s->vy * dt;

		if (s->x < 0.0f) {
			s->x = 0.0f;
			s->vx *= -1.0f;
		}

		if (s->x > (float)w) {
			s->x = (float)w;
			s->vx *= -1.0f;
		}

		if (s->y < 0.0f) {
			s->y = 0.0f;
			s->vy *= -1.0f;
		}

		if (s->y > (float)h) {
			s->y = (float)h;
			s->vy *= -1.0f;
		}
	}
}

static SDL_GPUShader *load_shader_spirv(SDL_GPUDevice *device,
                                        const char *path,
                                        SDL_GPUShaderStage stage,
                                        Uint32 num_uniform_buffers,
                                        Uint32 num_storage_buffers)
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

	Uint8 *code = (Uint8 *)malloc((size_t)size);
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

	SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);

	free(code);

	if (!shader) {
		fprintf(stderr, "SDL_CreateGPUShader(%s) failed: %s\n", path, SDL_GetError());
		return NULL;
	}

	return shader;
}

static void init_voronoi_pipeline(void)
{
	SDL_GPUShader *vert = load_shader_spirv(
		gpu_device,
		"voronoi.vert.spv",
		SDL_GPU_SHADERSTAGE_VERTEX,
		0,
		0);

	SDL_GPUShader *frag = load_shader_spirv(
		gpu_device,
		"voronoi.frag.spv",
		SDL_GPU_SHADERSTAGE_FRAGMENT,
		1, // one fragment uniform buffer
		1  // one fragment storage buffer
	);

	if (!vert || !frag) {
		fprintf(stderr, "Failed to load voronoi shaders\n");
		exit(1);
	}

	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
	color_target_desc.blend_state.enable_blend = false;
	color_target_desc.blend_state.color_write_mask =
		SDL_GPU_COLORCOMPONENT_R |
		SDL_GPU_COLORCOMPONENT_G |
		SDL_GPU_COLORCOMPONENT_B |
		SDL_GPU_COLORCOMPONENT_A;

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

	voronoi_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipeline_info);
	if (!voronoi_pipeline) {
		fprintf(stderr, "SDL_CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_ReleaseGPUShader(gpu_device, vert);
	SDL_ReleaseGPUShader(gpu_device, frag);
}

static void init_voronoi_seed_buffer(void)
{
	SDL_GPUBufferCreateInfo buffer_info = {};
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	buffer_info.size = sizeof(VoronoiGpuSeed) * VORONOI_MAX_SEEDS;
	buffer_info.props = 0;

	voronoi_seed_buffer = SDL_CreateGPUBuffer(gpu_device, &buffer_info);
	if (!voronoi_seed_buffer) {
		fprintf(stderr, "SDL_CreateGPUBuffer failed: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(VoronoiGpuSeed) * VORONOI_MAX_SEEDS;
	transfer_info.props = 0;

	voronoi_seed_transfer_buffer =
		SDL_CreateGPUTransferBuffer(gpu_device, &transfer_info);

	if (!voronoi_seed_transfer_buffer) {
		fprintf(stderr, "SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());
		exit(1);
	}
}

static void upload_voronoi_seed_buffer(SDL_GPUCommandBuffer *command_buffer)
{
	VoronoiGpuSeed *mapped =
		(VoronoiGpuSeed *)SDL_MapGPUTransferBuffer(gpu_device,
		                                           voronoi_seed_transfer_buffer,
		                                           true);

	if (!mapped) {
		fprintf(stderr, "SDL_MapGPUTransferBuffer failed: %s\n", SDL_GetError());
		return;
	}

	memcpy(mapped,
	       voronoi_gpu_seeds,
	       sizeof(VoronoiGpuSeed) * VORONOI_MAX_SEEDS);

	SDL_UnmapGPUTransferBuffer(gpu_device, voronoi_seed_transfer_buffer);

	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

	SDL_GPUTransferBufferLocation src = {};
	src.transfer_buffer = voronoi_seed_transfer_buffer;
	src.offset = 0;

	SDL_GPUBufferRegion dst = {};
	dst.buffer = voronoi_seed_buffer;
	dst.offset = 0;
	dst.size = sizeof(VoronoiGpuSeed) * VORONOI_MAX_SEEDS;

	SDL_UploadToGPUBuffer(copy_pass, &src, &dst, true);

	SDL_EndGPUCopyPass(copy_pass);
}

static void prepare_voronoi_gpu_frame(SDL_GPUCommandBuffer *command_buffer,
                                      int window_w,
                                      int window_h,
                                      float dt)
{
	if (!voronoi.initialized) {
		init_voronoi_background(window_w, window_h);
	}

	if (voronoi_active_seed_count < 1) {
		voronoi_active_seed_count = 1;
	}

	if (voronoi_active_seed_count > VORONOI_MAX_SEEDS) {
		voronoi_active_seed_count = VORONOI_MAX_SEEDS;
	}

	voronoi_time += dt;

	update_voronoi_background(dt, window_w, window_h);

	for (int i = 0; i < VORONOI_MAX_SEEDS; i++) {
		VoronoiSeed *s = &voronoi.seeds[i];

		voronoi_gpu_seeds[i].pos[0] = s->x;
		voronoi_gpu_seeds[i].pos[1] = s->y;
		voronoi_gpu_seeds[i].pos[2] = 0.0f;
		voronoi_gpu_seeds[i].pos[3] = 0.0f;

		voronoi_gpu_seeds[i].color[0] = (float)s->r / 255.0f;
		voronoi_gpu_seeds[i].color[1] = (float)s->g / 255.0f;
		voronoi_gpu_seeds[i].color[2] = (float)s->b / 255.0f;
		voronoi_gpu_seeds[i].color[3] = 1.0f;
	}

	upload_voronoi_seed_buffer(command_buffer);
}

static void draw_voronoi_gpu(SDL_GPUCommandBuffer *command_buffer,
                             SDL_GPURenderPass *render_pass,
                             int window_w,
                             int window_h)
{
	VoronoiFragUniforms u = {};

	u.screen_time_count[0] = (float)window_w;
	u.screen_time_count[1] = (float)window_h;
	u.screen_time_count[2] = voronoi_time;
	u.screen_time_count[3] = (float)voronoi_active_seed_count;

	SDL_PushGPUFragmentUniformData(command_buffer, 0, &u, sizeof(u));

	SDL_GPUBuffer *storage_buffers[] = {
		voronoi_seed_buffer,
	};

	SDL_BindGPUGraphicsPipeline(render_pass, voronoi_pipeline);

	SDL_BindGPUFragmentStorageBuffers(render_pass,
	                                  0,
	                                  storage_buffers,
	                                  1);

	SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
}

static void render(void)
{
	ImGui::Render();

	ImDrawData *draw_data = ImGui::GetDrawData();

	bool is_minimized =
		draw_data->DisplaySize.x <= 0.0f ||
		draw_data->DisplaySize.y <= 0.0f;

	SDL_GPUCommandBuffer *command_buffer =
		SDL_AcquireGPUCommandBuffer(gpu_device);

	if (command_buffer == NULL) {
		fprintf(stderr, "SDL_AcquireGPUCommandBuffer failed: %s\n", SDL_GetError());
		return;
	}

	SDL_GPUTexture *swapchain_texture = NULL;
	Uint32 swapchain_w = 0;
	Uint32 swapchain_h = 0;

	if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer,
	                                           window,
	                                           &swapchain_texture,
	                                           &swapchain_w,
	                                           &swapchain_h)) {
		fprintf(stderr, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s\n", SDL_GetError());
		SDL_CancelGPUCommandBuffer(command_buffer);
		return;
	}

	if (swapchain_texture != NULL && !is_minimized) {
		ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

		float dt = ImGui::GetIO().DeltaTime;

		prepare_voronoi_gpu_frame(command_buffer,
		                          (int)swapchain_w,
		                          (int)swapchain_h,
		                          dt);

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

		SDL_GPURenderPass *render_pass =
			SDL_BeginGPURenderPass(command_buffer, &target_info, 1, NULL);

		draw_voronoi_gpu(command_buffer,
		                 render_pass,
		                 (int)swapchain_w,
		                 (int)swapchain_h);

		ImGui_ImplSDLGPU3_RenderDrawData(draw_data,
		                                 command_buffer,
		                                 render_pass);

		SDL_EndGPURenderPass(render_pass);
	}

	SDL_SubmitGPUCommandBuffer(command_buffer);
}

static void backend_exit(void)
{
	SDL_WaitForGPUIdle(gpu_device);

	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();

	ImPlot::DestroyContext();
	ImGui::DestroyContext();

	if (voronoi_pipeline) {
		SDL_ReleaseGPUGraphicsPipeline(gpu_device, voronoi_pipeline);
		voronoi_pipeline = NULL;
	}

	if (voronoi_seed_buffer) {
		SDL_ReleaseGPUBuffer(gpu_device, voronoi_seed_buffer);
		voronoi_seed_buffer = NULL;
	}

	if (voronoi_seed_transfer_buffer) {
		SDL_ReleaseGPUTransferBuffer(gpu_device, voronoi_seed_transfer_buffer);
		voronoi_seed_transfer_buffer = NULL;
	}

	SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
	SDL_DestroyGPUDevice(gpu_device);
	SDL_DestroyWindow(window);

	SDL_Quit();
}

static void new_frame(void)
{
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

static void check_for_exit(bool *done)
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
