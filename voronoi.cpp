#include <time.h>

#include "voronoi_seed.h"

typedef struct Voronoi_Seed {
	float x;
	float y;
	float vx;
	float vy;
	Uint8 r;
	Uint8 g;
	Uint8 b;
} Voronoi_Seed;

typedef struct Voronoi_Background {
	Voronoi_Seed seeds[VORONOI_SEED_COUNT];
} Voronoi_Background;

typedef struct Voronoi_Gpu_Seed {
	float pos[4];   // x, y, 0, 0
	float color[4]; // r, g, b, 1
} Voronoi_Gpu_Seed;

typedef struct Voronoi {
	SDL_GPUDevice *gpu_device;
	SDL_Window *window;
	SDL_GPUGraphicsPipeline *voronoi_pipeline;
	Voronoi_Background background;
	Voronoi_Gpu_Seed gpu_seeds[VORONOI_SEED_COUNT];
	SDL_GPUBuffer *seed_buffer;
	SDL_GPUTransferBuffer *seed_transfer_buffer;
} Voronoi;

static Voronoi voronoi;

static float rand_float01(void)
{
	return (float)rand() / (float)RAND_MAX;
}

static float rand_float_range(float min, float max)
{
	return min + (max - min) * rand_float01();
}

static SDL_GPUShader *load_shader_spirv(SDL_GPUDevice *device, const char *path, SDL_GPUShaderStage stage, Uint32 num_uniform_buffers, Uint32 num_storage_buffers)
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

	SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);

	if (!shader) {
		fprintf(stderr, "SDL_CreateGPUShader(%s) failed: %s\n", path, SDL_GetError());
		return NULL;
	}
	free(code);

	return shader;
}

static void init_voronoi_pipeline(void)
{
	SDL_GPUShader *vert = load_shader_spirv(voronoi.gpu_device, "voronoi.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
	SDL_GPUShader *frag = load_shader_spirv(voronoi.gpu_device, "voronoi.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);

	if (!vert || !frag) {
		fprintf(stderr, "Failed to load voronoi shaders\n");
		exit(1);
	}

	SDL_GPUColorTargetDescription color_target_desc = {};
	color_target_desc.format = SDL_GetGPUSwapchainTextureFormat(voronoi.gpu_device, voronoi.window);
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

	voronoi.voronoi_pipeline = SDL_CreateGPUGraphicsPipeline(voronoi.gpu_device, &pipeline_info);
	if (!voronoi.voronoi_pipeline) {
		fprintf(stderr, "SDL_CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_ReleaseGPUShader(voronoi.gpu_device, vert);
	SDL_ReleaseGPUShader(voronoi.gpu_device, frag);
}

static void init_voronoi_seed_buffer(void)
{
	SDL_GPUBufferCreateInfo buffer_info = {};
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	buffer_info.size = sizeof(Voronoi_Gpu_Seed) * VORONOI_SEED_COUNT;
	buffer_info.props = 0;

	voronoi.seed_buffer = SDL_CreateGPUBuffer(voronoi.gpu_device, &buffer_info);
	if (!voronoi.seed_buffer) {
		fprintf(stderr, "SDL_CreateGPUBuffer failed: %s\n", SDL_GetError());
		exit(1);
	}

	SDL_GPUTransferBufferCreateInfo transfer_info = {};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size = sizeof(Voronoi_Gpu_Seed) * VORONOI_SEED_COUNT;
	transfer_info.props = 0;

	voronoi.seed_transfer_buffer = SDL_CreateGPUTransferBuffer(voronoi.gpu_device, &transfer_info);

	if (!voronoi.seed_transfer_buffer) {
		fprintf(stderr, "SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());
		exit(1);
	}
}

void init_voronoi(SDL_GPUDevice *gpu_device, SDL_Window *window, int w, int h)
{
	voronoi.gpu_device = gpu_device;
	voronoi.window = window;
	srand(time(0));

	for (int i = 0; i < VORONOI_SEED_COUNT; i++) {
		Voronoi_Seed *s = &voronoi.background.seeds[i];

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

	init_voronoi_pipeline();
	init_voronoi_seed_buffer();
}

static void update_voronoi_background(float dt, int w, int h)
{
	if (dt > 0.033f) {
		dt = 0.033f;
	}

	for (int i = 0; i < VORONOI_SEED_COUNT; i++) {
		Voronoi_Seed *s = &voronoi.background.seeds[i];

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

static void upload_voronoi_seed_buffer(SDL_GPUCommandBuffer *command_buffer)
{
	Voronoi_Gpu_Seed *mapped = (Voronoi_Gpu_Seed *)SDL_MapGPUTransferBuffer(voronoi.gpu_device, voronoi.seed_transfer_buffer, true);

	if (!mapped) {
		fprintf(stderr, "SDL_MapGPUTransferBuffer failed: %s\n", SDL_GetError());
		return;
	}

	memcpy(mapped, voronoi.gpu_seeds, sizeof(Voronoi_Gpu_Seed) * VORONOI_SEED_COUNT);

	SDL_UnmapGPUTransferBuffer(voronoi.gpu_device, voronoi.seed_transfer_buffer);

	SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

	SDL_GPUTransferBufferLocation src = {};
	src.transfer_buffer = voronoi.seed_transfer_buffer;
	src.offset = 0;

	SDL_GPUBufferRegion dst = {};
	dst.buffer = voronoi.seed_buffer;
	dst.offset = 0;
	dst.size = sizeof(Voronoi_Gpu_Seed) * VORONOI_SEED_COUNT;

	SDL_UploadToGPUBuffer(copy_pass, &src, &dst, true);

	SDL_EndGPUCopyPass(copy_pass);
}

void prepare_voronoi_gpu_frame(SDL_GPUCommandBuffer *command_buffer, int window_w, int window_h, float dt)
{
	update_voronoi_background(dt, window_w, window_h);

	for (int i = 0; i < VORONOI_SEED_COUNT; i++) {
		Voronoi_Seed *s = &voronoi.background.seeds[i];

		voronoi.gpu_seeds[i].pos[0] = s->x;
		voronoi.gpu_seeds[i].pos[1] = s->y;
		voronoi.gpu_seeds[i].pos[2] = 0.0f;
		voronoi.gpu_seeds[i].pos[3] = 0.0f;

		voronoi.gpu_seeds[i].color[0] = (float)s->r / 255.0f;
		voronoi.gpu_seeds[i].color[1] = (float)s->g / 255.0f;
		voronoi.gpu_seeds[i].color[2] = (float)s->b / 255.0f;
		voronoi.gpu_seeds[i].color[3] = 1.0f;
	}

	upload_voronoi_seed_buffer(command_buffer);
}

void draw_voronoi_gpu(SDL_GPURenderPass *render_pass)
{
	SDL_GPUBuffer *storage_buffers[] = {
		voronoi.seed_buffer,
	};

	SDL_BindGPUGraphicsPipeline(render_pass, voronoi.voronoi_pipeline);

	SDL_BindGPUFragmentStorageBuffers(render_pass, 0, storage_buffers, 1);

	SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
}

void destroy_voronoi(void)
{
	if (voronoi.voronoi_pipeline) {
		SDL_ReleaseGPUGraphicsPipeline(voronoi.gpu_device, voronoi.voronoi_pipeline);
		voronoi.voronoi_pipeline = NULL;
	}

	if (voronoi.seed_buffer) {
		SDL_ReleaseGPUBuffer(voronoi.gpu_device, voronoi.seed_buffer);
		voronoi.seed_buffer = NULL;
	}

	if (voronoi.seed_transfer_buffer) {
		SDL_ReleaseGPUTransferBuffer(voronoi.gpu_device, voronoi.seed_transfer_buffer);
		voronoi.seed_transfer_buffer = NULL;
	}
}
