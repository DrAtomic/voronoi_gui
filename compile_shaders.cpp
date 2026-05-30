// copied from https://github.com/tsoding/voronoi-opengl

#include "glextloader.c"
#include <stdio.h>
#include <errno.h>

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
