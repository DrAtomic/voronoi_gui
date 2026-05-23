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

#include "voronoi.cpp"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 800

static SDL_Window *window;
static SDL_GPUDevice *gpu_device;

static const char *lib_plug_name = "./lib_plug.so";
static void *libplug;

static plug_init_t plug_init;
static plug_update_t plug_update;
static plug_pre_reload_t plug_pre_reload;
static plug_post_reload_t plug_post_reload;

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
	SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;

	window = SDL_CreateWindow("voronoi", (int)(WINDOW_WIDTH * main_scale), (int)(WINDOW_HEIGHT * main_scale), window_flags);

	if (window == NULL) {
		fprintf(stderr, "Error: SDL_CreateWindow(): %s\n", SDL_GetError());
		exit(1);
	}

	gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, NULL);

	if (gpu_device == NULL) {
		fprintf(stderr, "Error: SDL_CreateGPUDevice(): %s\n", SDL_GetError());
		exit(1);
	}

	if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
		fprintf(stderr, "Error: SDL_ClaimWindowForGPUDevice(): %s\n", SDL_GetError());
		exit(1);
	}

	SDL_SetGPUSwapchainParameters(gpu_device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_ShowWindow(window);
}

static void backend_init(void)
{
	init_sdl();

	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGui::StyleColorsDark();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui_ImplSDL3_InitForSDLGPU(window);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = gpu_device;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
	init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;

	ImGui_ImplSDLGPU3_Init(&init_info);
	SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
	init_voronoi(gpu_device, swapchain_format, WINDOW_WIDTH, WINDOW_HEIGHT);
}

static void render(void)
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

		prepare_voronoi_gpu_frame(gpu_device, command_buffer, (int)swapchain_w, (int)swapchain_h, dt);

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

		draw_voronoi_gpu(render_pass);

		ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);

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

	destroy_voronoi(gpu_device);

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
