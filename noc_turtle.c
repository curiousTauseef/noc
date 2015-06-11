/* noc_turtle library
 *
 * Copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "noc_turtle.h"

#define min(x, y) ((x) <= (y) ? (x) : (y))
#define max(x, y) ((x) >= (y) ? (x) : (y))


// Some matrix functions.

static void mat_set_identity(float mat[16])
{
    int i;
    memset(mat, 0, 16 * sizeof(float));
    for (i = 0; i < 4; i++)
        mat[i * 4 + i] = 1;
}

static void mat_scale(float m[16], float x, float y, float z)
{
    m[0] *= x;   m[4] *= y;   m[8]  *= z;
    m[1] *= x;   m[5] *= y;   m[9]  *= z;
    m[2] *= x;   m[6] *= y;   m[10] *= z;
    m[3] *= x;   m[7] *= y;   m[11] *= z;
}

static void mat_translate(float m[16], float x, float y, float z)
{
#define M(row,col)  m[row * 4 + col]
    int i;
    for (i = 0; i < 4; i++) {
       M(3, i) += M(0, i) * x + M(1, i) * y + M(2, i) * z;
    }
#undef M
}

static void mat_mult(float a[16], const float b[16])
{
    int i, j, k;
    float ret[16];
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            ret[j*4 + i] = 0.0;
            for (k = 0; k < 4; ++k) {
                ret[j*4 + i] += a[k*4 + i] * b[j*4 + k];
            }
        }
    }
    memcpy(a, ret,  sizeof(ret));
}

static void mat_rotate(float m[16], float a, float x, float y, float z)
{
    if (a == 0.0)
        return;
    float tmp[16];
    mat_set_identity(tmp);
    float s = sin(a);
    float c = cos(a);
#define M(row,col)  tmp[col * 4 + row]
    // Only support z axis rotations.
    if (x == 0.0 && y == 0.0 && z == 1.0) {
        M(0, 0) = c;
        M(1, 1) = c;
        M(0, 1) = -s;
        M(1, 0) = s;
    } else {
        assert(false);
    }
#undef M
    mat_mult(m, tmp);
}

static noctt_vec3_t mat_mul_vec(const float m[16], const noctt_vec3_t v)
{
    int i, j;
    float v4[4] = {v.x, v.y, v.z, 1};
    float ret[3];

    for (i = 0; i < 3; i++) {
        ret[i] = 0;
        for (j = 0; j < 4; j++) {
            ret[i] += m[j * 4 + i] * v4[j];
        }
    }
    return (noctt_vec3_t){ret[0], ret[1], ret[2]};
}

static void noctt_dead(noctt_turtle_t *ctx) { }

noctt_vec3_t noctt_get_pos(const noctt_turtle_t *ctx)
{
    noctt_vec3_t p = {0, 0, 0};
    return mat_mul_vec(ctx->mat, p);
}

void noctt_kill(noctt_turtle_t *ctx)
{
    ctx->func = noctt_dead;
    ctx->iflags |= NOCTT_FLAG_DONE;
    ctx->iflags &= ~NOCTT_FLAG_WAITING;
}

static int noctt_tr_iter_op(int *n_tot, const float **codes, int *nb)
{
    const float *c;
    int op;
    if (*n_tot == 0) return NOCTT_OP_END;
    c = *codes + *nb;
    *n_tot -= *nb;
    if (*n_tot == 0) return NOCTT_OP_END;

    assert(*c == NOCTT_OP_START);
    op = (int)c[1];
    assert(op >= 0 && op < NOCTT_OP_COUNT);
    if (op == NOCTT_OP_END) return NOCTT_OP_END;
    c += 2;
    *n_tot -= 2;
    for (*nb = 0; *nb < *n_tot && c[*nb] != NOCTT_OP_START; (*nb)++) {}
    *codes = c;
    return op;
}

static void scale(noctt_turtle_t *ctx, float x, float y, float z)
{
    mat_scale(ctx->mat, x, y, z);
    ctx->scale[0] *= x;
    ctx->scale[1] *= y;
}

static void scale_normalize(noctt_turtle_t *ctx)
{
    float x, y;
    x = ctx->scale[0];
    y = ctx->scale[1];
    if (y > x)
        scale(ctx, 1, x / y, 1);
    if (x > y)
        scale(ctx, y / x, 1, 1);
}

static void grow(noctt_turtle_t *ctx, float x, float y)
{
    float sx, sy, kx, ky;
    sx = ctx->scale[0] / ctx->prog->pixel_size;
    sy = ctx->scale[1] / ctx->prog->pixel_size;
    kx = (2 * x + sx) / sx;
    ky = (2 * y + sy) / sy;
    scale(ctx, kx, ky, 1);
}

static float mix(float x, float y, float t)
{
    return x * (1 - t) + y * t;
}

static float move_value(float x, float v, float range)
{
    float dst = v >= 0 ? range : 0;
    v = fabs(v);
    return mix(x, dst, v);
}

static inline float mod(float x, float y)
{
    while (x < 0) x += y;
    return fmod(x, y);
}

static float mix_angle(float x, float y, float t)
{
    float ret, tmp;
    x = mod(x, 360);
    y = mod(y, 360);
    if (x > y) {
        tmp = x;
        x = y;
        y = tmp;
        t = 1 - t;
    }
    if (y - x > 180) y -= 360;
    ret = mod(mix(x, y, t), 360);
    return ret;
}

static void flip(noctt_turtle_t *ctx, float a)
{
    a = a / 180 * M_PI;
    float x = cos(a);
    float y = sin(a);
    float m[16] = {
        x * x - y * y, 2 * x * y    , 0, 0,
        2 * x * y    , y * y - x * x, 0, 0,
        0            , 0            , 1, 0,
        0            , 0            , 0, 1};
    mat_mult(ctx->mat, m);
}

static int set_flags(int x, int mask, bool value)
{
    if (value)
        return x | mask;
    else
        return x & ~mask;
}

void noctt_tr(noctt_turtle_t *ctx, int n_tot, const float *codes)
{
    int nb = 0, op, c, i;
    while ((op = noctt_tr_iter_op(&n_tot, &codes, &nb)) != NOCTT_OP_END) {
        switch (op) {
        case NOCTT_OP_S:
            assert(nb >= 1 && nb <= 3);
            scale(ctx, codes[0],
                  nb > 1 ? codes[1] : codes[0],
                  nb > 2 ? codes[2] : 1);
            break;
        case NOCTT_OP_SAXIS:
            assert(nb == 2);
            assert(codes[0] >= 0 && codes[0] <= 2);
            scale(ctx, codes[0] == 0 ? codes[1] : 1,
                       codes[0] == 1 ? codes[1] : 1,
                       codes[0] == 2 ? codes[1] : 1);
            break;
        case NOCTT_OP_SN:
            assert(nb == 0);
            scale_normalize(ctx);
            break;
        case NOCTT_OP_G:
            assert(nb >= 1 && nb <= 2);
            grow(ctx, codes[0], nb > 1 ? codes[1] : codes[0]);
            break;
        case NOCTT_OP_X:
            assert(nb > 0 && nb <= 3);
            mat_translate(ctx->mat,
                          codes[0],
                          nb > 1 ? codes[1] : 0,
                          nb > 2 ? codes[2] : 0);
            break;
        case NOCTT_OP_R:
            assert(nb == 1);
            mat_rotate(ctx->mat, codes[0] / 180 * M_PI, 0, 0, 1);
            break;
        case NOCTT_OP_FLIP:
            assert(nb == 1);
            flip(ctx, codes[0]);
            break;
        case NOCTT_OP_HUE:
            assert(nb == 1 || nb == 2);
            if (nb == 1)
                ctx->color[0] = mod(ctx->color[0] + codes[0], 360);
            else
                ctx->color[0] = mix_angle(ctx->color[0], codes[1], codes[0]);
            break;
        case NOCTT_OP_SAT:
        case NOCTT_OP_LIGHT:
        case NOCTT_OP_A:
            assert(nb > 0 && nb <= 2);
            c = op - NOCTT_OP_HUE;
            if (nb == 1)
                ctx->color[c] = move_value(ctx->color[c], codes[0], 1);
            else
                ctx->color[c] = mix(ctx->color[c], codes[1], codes[0]);
            break;
        case NOCTT_OP_HSL:
            assert(nb == 3 || nb == 4);
            if (nb == 3) {
                ctx->color[0] = mod(ctx->color[0] + codes[0], 360);
                ctx->color[1] = move_value(ctx->color[1], codes[1], 1);
                ctx->color[2] = move_value(ctx->color[2], codes[2], 1);
            } else {
                ctx->color[0] = mix_angle(ctx->color[0], codes[1], codes[0]);
                ctx->color[1] = mix(ctx->color[1], codes[2], codes[0]);
                ctx->color[2] = mix(ctx->color[2], codes[3], codes[0]);
            }
            break;
        case NOCTT_OP_FLAG:
            assert(nb == 1 || (nb % 2) == 0);
            for (i = 0; i < nb; i += 2) {
                ctx->flags = set_flags(ctx->flags, codes[i],
                                   nb > 1 ? codes[i + 1] : 1);
            }
            break;
        case NOCTT_OP_VAR:
            assert(nb % 2 == 0);
            for (i = 0; i < nb; i += 2) {
                assert(codes[i] >= 0 &&
                       codes[i] < (sizeof(ctx->vars) / sizeof(ctx->vars[0])));
                ctx->vars[(int)codes[i]] = codes[i + 1];
            }
            break;
        default:
            assert(0);
        }
    }
}

void noctt_clone(noctt_turtle_t *ctx, int mode, int n, const float *ops)
{
    int i;
    noctt_turtle_t *new_turtle = NULL;
    assert(!(ctx->iflags & NOCTT_FLAG_WAITING));
    ctx->iflags &= ~NOCTT_FLAG_JUST_CLONED;
    for (i = 0; i < ctx->prog->nb; i++) {
        if (ctx->prog->turtles[i].func == NULL) {
            new_turtle = &ctx->prog->turtles[i];
            *new_turtle = *ctx;
            new_turtle->iflags |= NOCTT_FLAG_JUST_CLONED;
            noctt_tr(new_turtle, n, ops);
            if (mode == 1) {
                ctx->iflags |= NOCTT_FLAG_WAITING;
                ctx->wait = i;
            }
            ctx->prog->active++;
            break;
        }
    }
}

noctt_prog_t *noctt_prog_create(noctt_rule_func_t rule, int nb, int seed,
                                float *mat, float pixel_size)
{
    noctt_prog_t *proc;
    noctt_turtle_t *ctx;
    proc = (noctt_prog_t*)
                calloc(1, sizeof(*proc) + nb * sizeof(*proc->turtles));
    proc->nb = nb;
    proc->rand_next = seed;
    assert(pixel_size);
    proc->pixel_size = pixel_size;
    // Init first context.
    proc->active = 1;
    ctx = &proc->turtles[0];
    ctx->color[3] = 1;
    ctx->func = rule;
    ctx->prog = proc;
    mat_set_identity(ctx->mat);
    if (mat)
        mat_mult(ctx->mat, mat);
    ctx->scale[0] = sqrt(mat[0] * mat[0] + mat[1] * mat[1] + mat[2] * mat[2]);
    ctx->scale[1] = sqrt(mat[4] * mat[4] + mat[5] * mat[5] + mat[6] * mat[6]);
    proc->min_scale = 0.25;

    return proc;
}

void noctt_prog_delete(noctt_prog_t *proc)
{
    free(proc);
}

static noctt_turtle_t *get_wait(const noctt_turtle_t *ctx)
{
    return (ctx->iflags & NOCTT_FLAG_WAITING) ? &ctx->prog->turtles[ctx->wait]
                                              : NULL;
}

static void assert_can_remove(const noctt_turtle_t *ctx)
{
#ifdef NDEBUG
    return;
#endif
    int i;
    for (i = 0; i < ctx->prog->nb; i++) {
        assert(!ctx->prog->turtles[i].func ||
                get_wait(&ctx->prog->turtles[i]) != ctx);
    }
}

static bool iter_context(noctt_turtle_t *ctx)
{
    if (ctx->func == noctt_dead) {
        assert_can_remove(ctx);
        ctx->func = NULL;
        ctx->prog->active--;
    }

    if (!ctx->func)
        ctx->iflags |= NOCTT_FLAG_DONE;

    if (ctx->iflags & NOCTT_FLAG_DONE)
        return true;

    if (get_wait(ctx) && (get_wait(ctx)->func == noctt_dead))
        ctx->iflags &= ~NOCTT_FLAG_WAITING;
    if (get_wait(ctx)) {
        if (get_wait(ctx)->iflags & NOCTT_FLAG_DONE)
            ctx->iflags |= NOCTT_FLAG_DONE;
        return false;
    }

    if (    fabs(ctx->scale[0]) <= ctx->prog->min_scale ||
            fabs(ctx->scale[0]) <= ctx->prog->min_scale) {
        noctt_kill(ctx);
        return true;
    }

    ctx->func(ctx);
    assert(ctx->func);
    ctx->time += 1;
    return true;
}

void noctt_prog_iter(noctt_prog_t *proc)
{
    int i;
    bool keep_going = true;

    for (i = 0; i < proc->nb; i++)
        proc->turtles[i].iflags &= ~NOCTT_FLAG_DONE;

    while (keep_going) {
        keep_going = false;
        for (i = 0; i < proc->nb; i++) {
            iter_context(&proc->turtles[i]);
            if (!(proc->turtles[i].iflags & NOCTT_FLAG_DONE))
                keep_going = true;
        }
    }
}

int noctt_rand(noctt_turtle_t *ctx)
{
    ctx->prog->rand_next = ctx->prog->rand_next * 1103515245 + 12345;
    return((unsigned)(ctx->prog->rand_next/65536) % 32768);
}

float noctt_frand(noctt_turtle_t *ctx, float min, float max)
{
    return min + (noctt_rand(ctx) % 4096) / 4096. * (max - min);
}

bool noctt_brand(noctt_turtle_t *ctx, float x)
{
    return noctt_frand(ctx, 0, 1) <= x;
}

float noctt_pm(noctt_turtle_t *ctx, float x, float a)
{
    return noctt_frand(ctx, x - a, x + a);
}

static void render(const noctt_turtle_t *ctx, int n, const noctt_vec3_t *poly,
                   const float color[4], unsigned int flags)
{
    if (!ctx->prog->render_callback) {
        printf("ERROR: need to set a render callback\n");
        assert(0);
    }
    ctx->prog->render_callback(n, poly, color, flags,
                               ctx->prog->render_callback_data);
}

void noctt_poly(const noctt_turtle_t *ctx, int n, const noctt_vec3_t *poly)
{
    noctt_vec3_t *points = (noctt_vec3_t*)malloc(n * sizeof(*points));
    int i;
    for (i = 0; i < n; i++)
        points[i] = mat_mul_vec(ctx->mat, poly[i]);
    render(ctx, n, points, ctx->color, ctx->flags);
    free(points);
}

void noctt_square(const noctt_turtle_t *ctx)
{
    noctt_vec3_t poly[4] = {
        {-0.5, -0.5}, {+0.5, -0.5}, {+0.5, +0.5}, {-0.5, +0.5}
    };
    noctt_poly(ctx, 4, poly);
}

void noctt_rsquare(const noctt_turtle_t *ctx, float c)
{
    const int n = 8;
    float sx, sy, sm, rx, ry, r, aa;
    int a, i;
    noctt_vec3_t *poly;

    sx = ctx->scale[0];
    sy = ctx->scale[1];
    sm = min(sx, sy);
    r = max((sm - c) / 2, 0);
    rx = r / sx;
    ry = r / sy;
    const float d[][2] = {{+0.5 - rx, +0.5 - ry},
                          {-0.5 + rx, +0.5 - ry},
                          {-0.5 + rx, -0.5 + ry},
                          {+0.5 - rx, -0.5 + ry}};
    poly = (noctt_vec3_t*)calloc(4 * n, sizeof(*poly));
    for (i = 0, a = 0; i < 4 * n; i++) {
        aa = a * M_PI / (2 * (n - 1));
        poly[i].x = rx * cos(aa) + d[i / n][0];
        poly[i].y = ry * sin(aa) + d[i / n][1];
        if ((i % n) != (n - 1)) a++;
    }
    noctt_poly(ctx, 4 * n, poly);
    free(poly);
}

void noctt_circle(const noctt_turtle_t *ctx)
{
    static const int CIRCLE_NB = 32;
    static noctt_vec3_t *poly = NULL;
    int i;
    if (!poly) {
        poly = (noctt_vec3_t*)calloc(CIRCLE_NB, sizeof(*poly));
        for (i = 0; i < CIRCLE_NB; i++) {
            poly[i].x = 0.5f * cos(2 * M_PI * i / CIRCLE_NB);
            poly[i].y = 0.5f * sin(2 * M_PI * i / CIRCLE_NB);
        }
    }
    noctt_poly(ctx, CIRCLE_NB, poly);
}

void noctt_star(const noctt_turtle_t *ctx, int n, float t, float c)
{
    float a;
    int i;
    noctt_vec3_t *p;

    p = (noctt_vec3_t*)calloc((2 + n * 2), sizeof(*p));
    p[0].x = 0;
    p[0].y = 0;
    // The branch points.
    for (i = 0; i < n + 1; i++) {
        a = i * 2 * M_PI / n;
        p[1 + 2 * i].x = 0.5 * cos(a);
        p[1 + 2 * i].y = 0.5 * sin(a);
    }
    // The middle points.
    c = (c + 1) / 2;
    for (i = 0; i < n; i++) {
        p[1 + 2 * i + 1].x = mix(
                mix(p[1 + 2 * i].x, p[1 + 2 * (i + 1)].x, c),
                0, t);
        p[1 + 2 * i + 1].y = mix(
                mix(p[1 + 2 * i].y, p[1 + 2 * (i + 1)].y, c),
                0, t);
    }
    noctt_poly(ctx, 2 + n * 2, p);
    free(p);
}
