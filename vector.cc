#include <iostream>
#include <fstream>
#include <string>
#include <zlib.h>
#include "vector_tile.pb.h"

#define XMAX 4096
#define YMAX 4096

extern "C" {
	#include "graphics.h"
	#include "clip.h"
}

struct line {
	int x0;
	int y0;
	int x1;
	int y1;
};

struct point {
	int x;
	int y;
};

struct pointlayer {
	struct point *points;
	int npoints;
	int npalloc;
};

class env {
public:
	mapnik::vector::tile tile;
	mapnik::vector::tile_layer *layer;
	mapnik::vector::tile_feature *feature;

	int x;
	int y;

	int cmd_idx;
	int cmd;
	int length;

	struct line *lines;
	int nlines;
	int nlalloc;

	struct pointlayer *pointlayers;
	unsigned char *used;
};

#define MOVE_TO 1
#define LINE_TO 2
#define CLOSE_PATH 7
#define CMD_BITS 3

// because with defaults, 16 overlapping dots reach full brightness
#define LAYERS 16

struct graphics {
	int width;
	int height;
	env *e;
};

struct graphics *graphics_init(int width, int height) {
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	env *e = new env;

	e->nlalloc = 1024;
	e->nlines = 0;
	e->lines = (struct line *) malloc(e->nlalloc * sizeof(struct line));

	e->pointlayers = (struct pointlayer *) malloc(LAYERS * sizeof(struct pointlayer));
	e->used = (unsigned char *) malloc(256 * 256 * sizeof(unsigned char));
	memset(e->used, '\0', 256 * 256 * sizeof(unsigned char));

	int i;
	for (i = 0; i < LAYERS; i++) {
		e->pointlayers[i].npalloc = 1024;
		e->pointlayers[i].npoints = 0;
		e->pointlayers[i].points = (struct point *) malloc(e->pointlayers[i].npalloc * sizeof(struct point));

	}

	struct graphics *g = (struct graphics *) malloc(sizeof(struct graphics));
	g->e = e;
	g->width = width;
	g->height = height;

	return g;
}

// from mapnik-vector-tile/src/vector_tile_compression.hpp
static inline int compress(std::string const& input, std::string & output)
{
	z_stream deflate_s;
	deflate_s.zalloc = Z_NULL;
	deflate_s.zfree = Z_NULL;
	deflate_s.opaque = Z_NULL;
	deflate_s.avail_in = 0;
	deflate_s.next_in = Z_NULL;
	deflateInit(&deflate_s, Z_DEFAULT_COMPRESSION);
	deflate_s.next_in = (Bytef *)input.data();
	deflate_s.avail_in = input.size();
	size_t length = 0;
	do {
		size_t increase = input.size() / 2 + 1024;
		output.resize(length + increase);
		deflate_s.avail_out = increase;
		deflate_s.next_out = (Bytef *)(output.data() + length);
		int ret = deflate(&deflate_s, Z_FINISH);
		if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
			return -1;
		}
		length += (increase - deflate_s.avail_out);
	} while (deflate_s.avail_out == 0);
	deflateEnd(&deflate_s);
	output.resize(length);
	return 0;
}

static void op(env *e, int cmd, int x, int y);

void out(struct graphics *gc, int transparency, double gamma, int invert, int color, int color2, int saturate, int mask) {
	env *e = gc->e;

	e->layer = e->tile.add_layers();
	e->layer->set_name("lines");
	e->layer->set_version(1);
	e->layer->set_extent(XMAX);

	e->feature = e->layer->add_features();
	e->feature->set_type(mapnik::vector::tile::LineString);

	e->x = 0;
	e->y = 0;

	e->cmd_idx = -1;
	e->cmd = -1;
	e->length = 0;

	int i;
	for (i = 0; i < e->nlines; i++) {
		// printf("draw %d %d to %d %d\n", e->lines[i].x0, e->lines[i].y0, e->lines[i].x1, e->lines[i].y1);

		if (e->lines[i].x0 != e->x || e->lines[i].y0 != e->y || e->length == 0) {
			op(e, MOVE_TO, e->lines[i].x0, e->lines[i].y0);
		}

		op(e, LINE_TO, e->lines[i].x1, e->lines[i].y1);
	}

	if (e->cmd_idx >= 0) {
		//printf("old command: %d %d\n", e->cmd, e->length);
		e->feature->set_geometry(e->cmd_idx, 
			(e->length << CMD_BITS) |
			(e->cmd & ((1 << CMD_BITS) - 1)));
	}

	//////////////////////////////////

	e->layer = e->tile.add_layers();
	e->layer->set_name("points");
	e->layer->set_version(1);
	e->layer->set_extent(XMAX);

	int j;
	for (j = 0; j < LAYERS; j++) {
		if (e->pointlayers[j].npoints != 0) {
			e->feature = e->layer->add_features();
			e->feature->set_type(mapnik::vector::tile::LineString);

			e->x = 0;
			e->y = 0;

			e->cmd_idx = -1;
			e->cmd = -1;
			e->length = 0;

			for (i = 0; i < e->pointlayers[j].npoints; i++) {
				op(e, MOVE_TO, e->pointlayers[j].points[i].x, e->pointlayers[j].points[i].y);
				op(e, LINE_TO, e->pointlayers[j].points[i].x + 1, e->pointlayers[j].points[i].y);
			}

			if (e->cmd_idx >= 0) {
				e->feature->set_geometry(e->cmd_idx, 
					(e->length << CMD_BITS) |
					(e->cmd & ((1 << CMD_BITS) - 1)));
			}
		}
	}

	//////////////////////////////////

	std::string s;
	e->tile.SerializeToString(&s);

	std::string compressed;
	compress(s, compressed);

	std::cout << compressed;
}

static void op(env *e, int cmd, int x, int y) {
	// printf("%d %d,%d\n", cmd, x, y);
	// printf("from cmd %d to %d\n", e->cmd, cmd);

	if (cmd != e->cmd) {
		if (e->cmd_idx >= 0) {
			// printf("old command: %d %d\n", e->cmd, e->length);
			e->feature->set_geometry(e->cmd_idx, 
				(e->length << CMD_BITS) |
				(e->cmd & ((1 << CMD_BITS) - 1)));
		}

		e->cmd = cmd;
		e->length = 0;
		e->cmd_idx = e->feature->geometry_size();

		e->feature->add_geometry(0); // placeholder
	}

	if (cmd == MOVE_TO || cmd == LINE_TO) {
		int dx = x - e->x;
		int dy = y - e->y;
		// printf("new geom: %d %d\n", x, y);

		e->feature->add_geometry((dx << 1) ^ (dx >> 31));
		e->feature->add_geometry((dy << 1) ^ (dy >> 31));
		
		e->x = x;
		e->y = y;
		e->length++;
	} else if (cmd == CLOSE_PATH) {
		e->length++;
	}
}

int drawClip(double x0, double y0, double x1, double y1, struct graphics *gc, double bright, double hue, int antialias, double thick, struct tilecontext *tc) {
	double mult = XMAX / gc->width;
	int accept = clip(&x0, &y0, &x1, &y1, 0, 0, XMAX / mult, YMAX / mult);

	if (accept) {
		int xx0 = x0 * mult;
		int yy0 = y0 * mult;
		int xx1 = x1 * mult;
		int yy1 = y1 * mult;

		// Guarding against rounding error

		if (xx0 < 0) {
			xx0 = 0;
		}
		if (xx0 > XMAX - 1) {
			xx0 = XMAX - 1;
		}
		if (yy0 < 0) {
			yy0 = 0;
		}
		if (yy0 > YMAX - 1) {
			yy0 = YMAX - 1;
		}

		if (xx1 < 0) {
			xx1 = 0;
		}
		if (xx1 > XMAX - 1) {
			xx1 = XMAX - 1;
		}
		if (yy1 < 0) {
			yy1 = 0;
		}
		if (yy1 > YMAX - 1) {
			yy1 = YMAX - 1;
		}

		env *e = gc->e;

		if (xx0 != xx1 || yy0 != yy1) {
			if (e->nlines + 1 >= e->nlalloc) {
				e->nlalloc *= 2;
				e->lines = (struct line *) realloc((void *) e->lines, e->nlalloc * sizeof(struct line));
			}

			e->lines[e->nlines].x0 = xx0;
			e->lines[e->nlines].y0 = yy0;
			e->lines[e->nlines].x1 = xx1;
			e->lines[e->nlines].y1 = yy1;

			e->nlines++;
		}
	}

	return 0;
}

void drawPixel(double x, double y, struct graphics *gc, double bright, double hue, struct tilecontext *tc) {
	x += .5;
	y += .5;

	double mult = XMAX / gc->width;
	int xx = x * mult;
	int yy = y * mult;

	// Guarding against rounding error

	if (xx < 0) {
		xx = 0;
	}
	if (xx > XMAX - 1) {
		xx = XMAX - 1;
	}
	if (yy < 0) {
		yy = 0;
	}
	if (yy > YMAX - 1) {
		yy = YMAX - 1;
	}

	env *e = gc->e;

	int xu = x * 256 / gc->width;
	int yu = y * 256 / gc->height;
	if (xu < 0) {
		xu = 0;
	}
	if (xu > 255) {
		xu = 255;
	}
	if (yu < 0) {
		yu = 0;
	}
	if (yu > 255) {
		yu = 255;
	}

	int i = e->used[256 * yu + xu] % LAYERS;
	e->used[256 * yu + xu]++;

	if (e->pointlayers[i].npoints + 1 >= e->pointlayers[i].npalloc) {
		e->pointlayers[i].npalloc *= 2;
		e->pointlayers[i].points = (struct point *) realloc((void *) e->pointlayers[i].points, e->pointlayers[i].npalloc * sizeof(struct point));
	}

	e->pointlayers[i].points[e->pointlayers[i].npoints].x = xx;
	e->pointlayers[i].points[e->pointlayers[i].npoints].y = yy;

	e->pointlayers[i].npoints++;
}

void drawBrush(double x, double y, struct graphics *gc, double bright, double brush, double hue, int gaussian, struct tilecontext *tc) {
	drawPixel(x - .5, y - .5, gc, bright, hue, tc);
}
