/* SPDX-License-Identifier: GPL-3.0-or-later
 * Same executor as Android: real ES3 contexts, shared resources, indexed
 * full-panel composition and a CPU read only for verification. Not iOS proof. */
#include "host.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static EgsHost *host;
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}
static uint32_t bits(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}
static EgsReply call(uint32_t context, const uint8_t *upload, uint32_t size, unsigned count, ...) {
    uint8_t command[256];
    assert(count <= 64);
    va_list args;
    va_start(args, count);
    for (unsigned i = 0; i < count; i++)
        put(command + i * 4, va_arg(args, unsigned));
    va_end(args);
    EgsReply r = {0};
    if (!egs_host_call(host, context, command, count * 4, upload, size, &r)) {
        fprintf(stderr, "host error: %s\n", egs_host_error(host));
        abort();
    }
    return r;
}
#define CALL(c, ...)                                                                                                   \
    do {                                                                                                               \
        EgsReply r = call((c), NULL, 0, __VA_ARGS__);                                                                  \
        assert(!r.size);                                                                                               \
        egs_reply_close(&r);                                                                                           \
    } while (0)
static void check_error(uint32_t context, uint32_t expected) {
    EgsReply r = call(context, NULL, 0, 1, EGS_GET_ERROR);
    if (r.size != 4 || word(r.data) != expected)
        fprintf(stderr, "GL error 0x%x, expected 0x%x\n", r.size == 4 ? word(r.data) : 0xffffffffu, expected);
    assert(r.size == 4 && word(r.data) == expected);
    egs_reply_close(&r);
}
static void source(uint32_t shader, const char *text) {
    EgsReply r = call(1, (const uint8_t *)text, strlen(text), 3, EGS_SHADER_SOURCE, shader, (unsigned)strlen(text));
    egs_reply_close(&r);
    CALL(1, 2, EGS_SHADER_COMPILE, shader);
    r = call(1, NULL, 0, 3, EGS_SHADER_PARAMETER, shader, GL_COMPILE_STATUS);
    if (!word(r.data)) {
        EgsReply log = call(1, NULL, 0, 3, EGS_SHADER_LOG, shader, 1024u);
        fprintf(stderr, "%.*s\n", (int)(log.size - 4), log.data + 4);
        abort();
    }
    egs_reply_close(&r);
}
static uint32_t location(const char *text) {
    uint8_t command[256];
    uint32_t n = strlen(text);
    put(command, EGS_UNIFORM_QUERY);
    put(command + 4, 100);
    put(command + 8, n);
    memcpy(command + 12, text, n);
    EgsReply r = {0};
    assert(egs_host_call(host, 1, command, n + 12, NULL, 0, &r));
    uint32_t v = word(r.data);
    assert(v != UINT32_MAX);
    egs_reply_close(&r);
    return v;
}
static void draw_setup(uint32_t c) {
    CALL(c, 2, EGS_PROGRAM_USE, 100u);
    CALL(c, 3, EGS_BUFFER_BIND, GL_ARRAY_BUFFER, 200u);
    CALL(c, 8, EGS_ATTRIB_POINTER, 0u, 2u, GL_FLOAT, 0u, 16u, 0u, 0u);
    CALL(c, 8, EGS_ATTRIB_POINTER, 1u, 2u, GL_FLOAT, 0u, 16u, 8u, 0u);
    CALL(c, 3, EGS_ATTRIB_ENABLE, 0u, 1u);
    CALL(c, 3, EGS_ATTRIB_ENABLE, 1u, 1u);
    CALL(c, 3, EGS_BUFFER_BIND, GL_ELEMENT_ARRAY_BUFFER, 201u);
    CALL(c, 2, EGS_TEXTURE_ACTIVE, GL_TEXTURE0);
    CALL(c, 3, EGS_TEXTURE_BIND, GL_TEXTURE_2D, 300u);
    CALL(c, 5, EGS_VIEWPORT, 0u, 0u, 828u, 1792u);
}
static void framebuffer(uint32_t c) {
    CALL(c, 4, EGS_OBJECT_CREATE, EGS_FRAMEBUFFER, 400u, 0u);
    CALL(c, 3, EGS_FRAMEBUFFER_BIND, GL_FRAMEBUFFER, 400u);
    CALL(c, 5, EGS_SURFACE_ATTACH, 700u, 0u, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0);
    EgsReply r = call(c, NULL, 0, 2, EGS_FRAMEBUFFER_STATUS, GL_FRAMEBUFFER);
    assert(word(r.data) == GL_FRAMEBUFFER_COMPLETE);
    egs_reply_close(&r);
}
static void render(uint32_t c) {
    CALL(c, 7, EGS_DRAW_ELEMENTS, GL_TRIANGLES, 6u, GL_UNSIGNED_SHORT, 0u, 0u, 1u);
    check_error(c, GL_NO_ERROR);
}
static void pixels(uint32_t c) {
    EgsReply r = call(c, NULL, 0, 7, EGS_SURFACE_READ, 700u, 0u, 0u, 0u, 828u, 1792u);
    assert(r.size == 828u * 1792u * 4);
    // The entire indexed quad samples one constant BGRA source, with alpha.
    for (uint32_t i = 0; i < r.size; i += 4) {
        assert(r.data[i] == 220 && r.data[i + 1] == 45 && r.data[i + 2] == 7 && r.data[i + 3] == 255);
    }
    egs_reply_close(&r);
}
/* EGS_DRAW_CLIENT body: 8 fixed words then k attribute records. */
static uint32_t client_command(uint8_t *command, uint32_t first, uint32_t count, uint32_t index_type,
                               uint32_t index_offset, unsigned k, const uint32_t (*attribs)[6]) {
    uint32_t head[8] = {EGS_DRAW_CLIENT, GL_TRIANGLES, first, count, index_type, index_offset, 1u, k};
    for (unsigned i = 0; i < 8; i++)
        put(command + i * 4, head[i]);
    for (unsigned i = 0; i < k; i++)
        for (unsigned j = 0; j < 6; j++)
            put(command + 32 + i * 24 + j * 4, attribs[i][j]);
    return 32 + k * 24;
}
static void client_draw(uint32_t c, const uint8_t *data, uint32_t size, uint32_t first, uint32_t count,
                        uint32_t index_type, uint32_t index_offset, unsigned k, const uint32_t (*attribs)[6],
                        uint32_t expected_error) {
    uint8_t command[32 + 16 * 24];
    uint32_t n = client_command(command, first, count, index_type, index_offset, k, attribs);
    EgsReply r = {0};
    if (!egs_host_call(host, c, command, n, data, size, &r)) {
        fprintf(stderr, "host error: %s\n", egs_host_error(host));
        abort();
    }
    assert(!r.size);
    egs_reply_close(&r);
    check_error(c, expected_error);
}
static void clear_target(uint32_t c) {
    CALL(c, 5, EGS_CLEAR_COLOR, bits(0), bits(0), bits(0), bits(1));
    CALL(c, 2, EGS_CLEAR, GL_COLOR_BUFFER_BIT);
    EgsReply r = call(c, NULL, 0, 7, EGS_SURFACE_READ, 700u, 0u, 400u, 900u, 1u, 1u);
    assert(r.size == 4 && !r.data[0] && !r.data[1] && !r.data[2]);
    egs_reply_close(&r);
}
/* CoreAnimation-style draws from memory: no VBO for these attributes. */
static void client_arrays(uint32_t c) {
    // Interleaved position+uv after 12 bytes of unrelated data; then indices.
    float vertices[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    uint8_t data[12 + sizeof(vertices) + 12] = {0};
    for (unsigned i = 0; i < sizeof(vertices) / 4; i++)
        put(data + 12 + i * 4, bits(vertices[i]));
    static const uint8_t ib[] = {0, 0, 1, 0, 2, 0, 2, 0, 1, 0, 3, 0};
    memcpy(data + 12 + sizeof(vertices), ib, sizeof(ib));
    const uint32_t interleaved[2][6] = {{0, 2, GL_FLOAT, 0, 16, 12}, {1, 2, GL_FLOAT, 0, 16, 20}};
    const uint32_t vertices_end = 12 + sizeof(vertices);
    // Indexed, both attributes from the upload.
    clear_target(c);
    client_draw(c, data, sizeof(data), 0, 6, GL_UNSIGNED_SHORT, vertices_end, 2, interleaved, GL_NO_ERROR);
    pixels(c);
    // Non-indexed strip of two triangles: vertices 0,1,2 and 1,3,2 -> as arrays 0..5 needs 6 verts;
    // use the same 4 vertices drawn as GL_TRIANGLES over an expanded copy.
    float expanded[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, -1, 1, 0, 1, 1, -1, 1, 0, 1, 1, 1, 1};
    uint8_t flat[sizeof(expanded)];
    for (unsigned i = 0; i < sizeof(expanded) / 4; i++)
        put(flat + i * 4, bits(expanded[i]));
    const uint32_t tight[2][6] = {{0, 2, GL_FLOAT, 0, 16, 0}, {1, 2, GL_FLOAT, 0, 16, 8}};
    clear_target(c);
    client_draw(c, flat, sizeof(flat), 0, 6, 0, 0, 2, tight, GL_NO_ERROR);
    pixels(c);
    // The VBO pointers set by draw_setup are back: an ordinary draw still works.
    clear_target(c);
    render(c);
    pixels(c);
    // One client attribute, the other still from VBO 200 (same layout).
    const uint32_t only_uv[1][6] = {{1, 2, GL_FLOAT, 0, 16, 8}};
    clear_target(c);
    client_draw(c, flat, sizeof(flat), 0, 6, 0, 0, 1, only_uv, GL_INVALID_OPERATION); // VBO has 4 vertices, not 6
    client_draw(c, flat, sizeof(flat), 0, 4, 0, 0, 1, only_uv, GL_NO_ERROR);
    // Bounds: every rejected draw leaves GL untouched and reports an error.
    client_draw(c, flat, sizeof(flat), 0, 7, 0, 0, 2, tight, GL_INVALID_OPERATION);   // vertex 6 past the data
    client_draw(c, flat, sizeof(flat), 1, 6, 0, 0, 2, tight, GL_INVALID_OPERATION);   // first shifts past the end
    const uint32_t late[2][6] = {{0, 2, GL_FLOAT, 0, 16, 92}, {1, 2, GL_FLOAT, 0, 16, 8}};
    client_draw(c, flat, sizeof(flat), 0, 1, 0, 0, 2, late, GL_INVALID_OPERATION);    // 92+8 > 96
    const uint32_t twice[2][6] = {{0, 2, GL_FLOAT, 0, 16, 0}, {0, 2, GL_FLOAT, 0, 16, 8}};
    client_draw(c, flat, sizeof(flat), 0, 6, 0, 0, 2, twice, GL_INVALID_VALUE);       // duplicate index
    const uint32_t wide[2][6] = {{0, 5, GL_FLOAT, 0, 16, 0}, {1, 2, GL_FLOAT, 0, 16, 8}};
    client_draw(c, flat, sizeof(flat), 0, 6, 0, 0, 2, wide, GL_INVALID_VALUE);        // 5 components
    const uint32_t odd[2][6] = {{0, 2, 0x1234u, 0, 16, 0}, {1, 2, GL_FLOAT, 0, 16, 8}};
    client_draw(c, flat, sizeof(flat), 0, 6, 0, 0, 2, odd, GL_INVALID_VALUE);         // unknown type
    uint8_t far_index[sizeof(data)];
    memcpy(far_index, data, sizeof(data));
    far_index[vertices_end + 10] = 4; // last u16 index -> vertex 4 of 4
    far_index[vertices_end + 11] = 0;
    client_draw(c, far_index, sizeof(far_index), 0, 6, GL_UNSIGNED_SHORT, vertices_end, 2, interleaved,
                GL_INVALID_OPERATION);
    client_draw(c, data, sizeof(data), 0, 6, GL_UNSIGNED_SHORT, vertices_end + 1, 2, interleaved,
                GL_INVALID_OPERATION); // misaligned indices
    client_draw(c, data, sizeof(data), 0, 7, GL_UNSIGNED_SHORT, vertices_end, 2, interleaved,
                GL_INVALID_OPERATION); // indices past the upload
    client_draw(c, data, sizeof(data), 0, 6, GL_INT, vertices_end, 2, interleaved, GL_INVALID_VALUE); // index type
    // A malformed record count or a missing upload never reaches GL.
    uint8_t command[32 + 16 * 24];
    uint32_t n = client_command(command, 0, 6, 0, 0, 2, tight);
    EgsReply r = {0};
    assert(!egs_host_call(host, c, command, n - 4, flat, sizeof(flat), &r));
    egs_reply_close(&r);
    put(command + 28, 17);
    assert(!egs_host_call(host, c, command, n, flat, sizeof(flat), &r)); // 17 records claimed, 2 sent
    egs_reply_close(&r);
    put(command + 28, 2);
    assert(egs_host_call(host, c, command, n, NULL, 0, &r));
    egs_reply_close(&r);
    check_error(c, GL_INVALID_VALUE);
    // Batched: two client draws with separate slices of one upload.
    uint8_t blob[2 * sizeof(flat)];
    memcpy(blob, flat, sizeof(flat));
    memcpy(blob + sizeof(flat), flat, sizeof(flat));
    uint8_t batch[8 + 2 * (12 + 80)];
    put(batch, EGS_BATCH);
    put(batch + 4, 2);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *e = batch + 8 + i * 92;
        put(e, 80);
        put(e + 4, i * (uint32_t)sizeof(flat));
        put(e + 8, sizeof(flat));
        client_command(e + 12, 0, 6, 0, 0, 2, tight);
    }
    clear_target(c);
    assert(egs_host_call(host, c, batch, sizeof(batch), blob, sizeof(blob), &r) && !r.size);
    egs_reply_close(&r);
    check_error(c, GL_NO_ERROR);
    pixels(c);
    // Ordinary indexed draw from the guest's own element buffer is unaffected.
    clear_target(c);
    render(c);
    pixels(c);
}
static void batch_checks(uint32_t c) {
    CALL(c, 5, EGS_CLEAR_COLOR, bits(1), bits(0), bits(0), bits(1));
    uint8_t batch[100] = {0};
    put(batch, EGS_BATCH);
    put(batch + 4, 2);
    put(batch + 8, 20);
    put(batch + 20, EGS_CLEAR_COLOR);
    put(batch + 24, bits(0));
    put(batch + 28, bits(1));
    put(batch + 32, bits(0));
    put(batch + 36, bits(1));
    put(batch + 40, 4);
    put(batch + 52, 0xdeadbeef);
    EgsReply r = {0};
    assert(!egs_host_call(host, c, batch, 56, NULL, 0, &r));
    egs_reply_close(&r);
    CALL(c, 2, EGS_CLEAR, GL_COLOR_BUFFER_BIT);
    r = call(c, NULL, 0, 7, EGS_READ_PIXELS, 0u, 0u, 1u, 1u, GL_RGBA, GL_UNSIGNED_BYTE);
    assert(r.size == 4 && r.data[0] == 255 && !r.data[1] && !r.data[2]);
    egs_reply_close(&r);
    // A valid batch executes scalar GL state in order without round-trip queries.
    put(batch + 52, EGS_CLEAR);
    put(batch + 40, 8);
    put(batch + 56, GL_COLOR_BUFFER_BIT);
    assert(egs_host_call(host, c, batch, 60, NULL, 0, &r));
    assert(!r.size);
    egs_reply_close(&r);
    r = call(c, NULL, 0, 7, EGS_READ_PIXELS, 0u, 0u, 1u, 1u, GL_RGBA, GL_UNSIGNED_BYTE);
    assert(r.size == 4 && !r.data[0] && r.data[1] == 255 && !r.data[2]);
    egs_reply_close(&r);
    // The native default VAO and GLuint names must remain virtualized.
    r = call(c, NULL, 0, 2, EGS_GET_INTEGER, GL_VERTEX_ARRAY_BINDING);
    assert(word(r.data) == 0);
    egs_reply_close(&r);
}
int main(void) {
    char error[192] = {0};
    host = egs_host_open(NULL, error);
    if (!host) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    printf("native backend: %s\n", egs_host_renderer(host));
    assert(egs_host_create(host, 1, 0, 828, 1792));
    assert(egs_host_create(host, 2, 1, 828, 1792));
    assert(egs_host_create(host, 3, 0, 64, 64));
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_SHADER, 10u, GL_VERTEX_SHADER);
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_SHADER, 11u, GL_FRAGMENT_SHADER);
    source(10, "#version 300 es\nlayout(location=0) in vec2 p;layout(location=1) in vec2 t;uniform mat4 matrix;out "
               "vec2 uv;void main(){gl_Position=matrix*vec4(p,0,1);uv=t;}");
    source(11, "#version 300 es\nprecision highp float;uniform sampler2D image;in vec2 uv;out vec4 color;void "
               "main(){color=texture(image,uv);}");
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_PROGRAM, 100u, 0u);
    CALL(1, 3, EGS_SHADER_ATTACH, 100u, 10u);
    CALL(1, 3, EGS_SHADER_ATTACH, 100u, 11u);
    CALL(1, 2, EGS_PROGRAM_LINK, 100u);
    EgsReply r = call(1, NULL, 0, 3, EGS_PROGRAM_PARAMETER, 100u, GL_LINK_STATUS);
    assert(word(r.data));
    egs_reply_close(&r);
    CALL(1, 2, EGS_PROGRAM_USE, 100u);
    uint8_t matrix[64] = {0};
    for (unsigned i = 0; i < 16; i++)
        put(matrix + i * 4, bits(i % 5 == 0 ? 1.f : 0.f));
    r = call(1, matrix, sizeof(matrix), 5, EGS_UNIFORM_MATRIX, location("matrix"), 4u, 1u, 0u);
    egs_reply_close(&r);
    uint8_t sampler[4] = {0};
    r = call(1, sampler, sizeof(sampler), 4, EGS_UNIFORM_IV, location("image"), 1u, 1u);
    egs_reply_close(&r);
    // Shared programs retain deleted shaders until detach; DELETE_STATUS remains queryable.
    CALL(1, 3, EGS_OBJECT_DELETE, EGS_SHADER, 10u);
    r = call(1, NULL, 0, 3, EGS_SHADER_PARAMETER, 10u, GL_DELETE_STATUS);
    assert(word(r.data));
    egs_reply_close(&r);
    float vertices[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    uint8_t vb[sizeof(vertices)];
    for (unsigned i = 0; i < sizeof(vertices) / 4; i++)
        put(vb + i * 4, bits(vertices[i]));
    uint8_t ib[] = {0, 0, 1, 0, 2, 0, 2, 0, 1, 0, 3, 0};
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_BUFFER, 200u, 0u);
    CALL(1, 3, EGS_BUFFER_BIND, GL_ARRAY_BUFFER, 200u);
    r = call(1, vb, sizeof(vb), 6, EGS_BUFFER_DATA, GL_ARRAY_BUFFER, (unsigned)sizeof(vb), 0u, GL_STATIC_DRAW, 1u);
    egs_reply_close(&r);
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_BUFFER, 201u, 0u);
    CALL(1, 3, EGS_BUFFER_BIND, GL_ELEMENT_ARRAY_BUFFER, 201u);
    r = call(1, ib, sizeof(ib), 6, EGS_BUFFER_DATA, GL_ELEMENT_ARRAY_BUFFER, (unsigned)sizeof(ib), 0u, GL_STATIC_DRAW,
             1u);
    egs_reply_close(&r);
    CALL(1, 6, EGS_SURFACE_CREATE, 600u, 0u, 17u, 19u, EGS_BGRA);
    uint8_t image[19 * 72 - 4];
    memset(image, 0x55, sizeof(image));
    for (unsigned y = 0; y < 19; y++)
        for (unsigned x = 0; x < 17; x++) {
            unsigned at = y * 72 + x * 4;
            image[at] = 7;
            image[at + 1] = 45;
            image[at + 2] = 220;
            image[at + 3] = 255;
        }
    r = call(1, image, sizeof(image), 12, EGS_SURFACE_UPLOAD, 600u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 17u, 19u, 72u);
    egs_reply_close(&r);
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_TEXTURE, 300u, 0u);
    CALL(1, 3, EGS_TEXTURE_BIND, GL_TEXTURE_2D, 300u);
    CALL(1, 4, EGS_SURFACE_TEXTURE, 600u, 0u, GL_TEXTURE_2D);
    CALL(1, 4, EGS_TEXTURE_PARAMETER, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_TEXTURE, 301u, 0u);
    CALL(1, 3, EGS_TEXTURE_BIND, GL_TEXTURE_2D, 301u);
    CALL(1, 4, EGS_SURFACE_TEXTURE, 600u, 0u, GL_TEXTURE_2D);
    CALL(1, 4, EGS_TEXTURE_PARAMETER, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    CALL(1, 3, EGS_TEXTURE_BIND, GL_TEXTURE_2D, 300u);
    r = call(1, NULL, 0, 3, EGS_TEXTURE_PARAMETER_QUERY, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER);
    assert(word(r.data) == GL_NEAREST);
    egs_reply_close(&r);
    // A framebuffer write through the second view must modify shared storage.
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_FRAMEBUFFER, 401u, 0u);
    CALL(1, 3, EGS_FRAMEBUFFER_BIND, GL_FRAMEBUFFER, 401u);
    CALL(1, 6, EGS_FRAMEBUFFER_TEXTURE, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 301u, 0u);
    CALL(1, 5, EGS_CLEAR_COLOR, bits(0), bits(0), bits(1), bits(1));
    CALL(1, 2, EGS_CLEAR, GL_COLOR_BUFFER_BIT);
    r = call(1, NULL, 0, 7, EGS_SURFACE_READ, 600u, 0u, 0u, 0u, 1u, 1u);
    assert(r.size == 4 && r.data[0] == 255 && !r.data[1] && !r.data[2] && r.data[3] == 255);
    egs_reply_close(&r);
    CALL(1, 7, EGS_SURFACE_COMMIT, 600u, 0u, 1u, 0u, 2u, 0u);
    r = call(1, image, sizeof(image), 12, EGS_SURFACE_UPLOAD, 600u, 0u, 2u, 0u, 3u, 0u, 0u, 0u, 17u, 19u, 72u);
    egs_reply_close(&r);
    CALL(1, 3, EGS_OBJECT_DELETE, EGS_FRAMEBUFFER, 401u);
    CALL(1, 3, EGS_OBJECT_DELETE, EGS_TEXTURE, 301u);
    CALL(1, 3, EGS_TEXTURE_BIND, GL_TEXTURE_2D, 300u);
    CALL(1, 4, EGS_TEXTURE_PARAMETER, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    CALL(1, 6, EGS_SURFACE_CREATE, 700u, 0u, 828u, 1792u, EGS_RGBA);
    framebuffer(1);
    draw_setup(1);
    render(1);
    pixels(1);
    client_arrays(1);
    draw_setup(2);
    framebuffer(2);
    render(2);
    pixels(2);
    // An unshared context must not resolve these virtual resources.
    CALL(3, 3, EGS_BUFFER_BIND, GL_ARRAY_BUFFER, 200u);
    check_error(3, GL_INVALID_OPERATION);
    // Deleting in one context keeps the other context's VAO reference alive.
    CALL(1, 3, EGS_OBJECT_DELETE, EGS_BUFFER, 200u);
    CALL(1, 4, EGS_OBJECT_CREATE, EGS_BUFFER, 200u, 0u);
    render(2);
    pixels(2);
    assert(egs_host_destroy(host, 1));
    render(2);
    pixels(2);
    batch_checks(2);
    // An oversized index must be caught before the GPU can read outside a VBO.
    uint8_t bad[] = {0xff, 0x7f};
    r = call(2, bad, 2, 6, EGS_BUFFER_SUB_DATA, GL_ELEMENT_ARRAY_BUFFER, 0u, 0u, 2u, 0u);
    egs_reply_close(&r);
    CALL(2, 7, EGS_DRAW_ELEMENTS, GL_TRIANGLES, 6u, GL_UNSIGNED_SHORT, 0u, 0u, 1u);
    check_error(2, GL_INVALID_OPERATION);
    // Every truncated fixed-size shape and unknown opcode is rejected safely.
    uint8_t malformed[64] = {0};
    for (unsigned op = 0; op < 160; op++) {
        put(malformed, op);
        for (unsigned n = 0; n < 4; n++) {
            EgsReply out = {0};
            assert(!egs_host_call(host, 2, malformed, n, NULL, 0, &out));
            egs_reply_close(&out);
        }
    }
    assert(egs_host_destroy(host, 2));
    assert(egs_host_destroy(host, 3));
    egs_host_close(host);
    puts("PASS ES3 shared contexts, 828x1792 indexed IOSurface composition, BGRA stride, retained resources, index "
         "bounds, client-side vertex arrays");
    return 0;
}
