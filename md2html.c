#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define da_reserve(da, expected_capacity)                                                          \
	do {                                                                                           \
		if ((expected_capacity) > (da)->capacity) {                                                \
			if ((da)->capacity == 0) {                                                             \
				(da)->capacity = 256;                                                              \
			}                                                                                      \
			while ((expected_capacity) > (da)->capacity) {                                         \
				(da)->capacity *= 2;                                                               \
			}                                                                                      \
			(da)->items = realloc((da)->items, (da)->capacity * sizeof(*(da)->items));             \
			assert((da)->items != NULL);                                                           \
		}                                                                                          \
	} while (0)

#define da_append(da, item)                                                                        \
	do {                                                                                           \
		da_reserve((da), (da)->count + 1);                                                         \
		(da)->items[(da)->count++] = (item);                                                       \
	} while (0)

#define da_append_many(da, new_items, new_items_count)                                             \
	do {                                                                                           \
		da_reserve((da), (da)->count + (new_items_count));                                         \
		memcpy((da)->items + (da)->count, (new_items), (new_items_count) * sizeof(*(da)->items));  \
		(da)->count += (new_items_count);                                                          \
	} while (0)

#define da_append_cstr(da, cstr) da_append_many((da), (cstr), strlen((cstr)))

typedef struct {
	char* items;
	size_t count;
	size_t capacity;
} StringBuilder;

typedef struct {
	char* items;
	size_t count;
} StringView;

bool read_entire_file(const char* filepath_cstr, StringBuilder* sb) {
	bool result = true;

	FILE* f = fopen(filepath_cstr, "rb");
	if (f == NULL)                 { result = false; goto defer; }
	if (fseek(f, 0, SEEK_END) < 0) { result = false; goto defer; }

#ifndef _WIN32
	long m = ftell(f);
#else
	long long m = _ftelli64(f);
#endif

	if (m < 0)                     { result = false; goto defer; }
	if (fseek(f, 0, SEEK_SET) < 0) { result = false; goto defer; }

	size_t new_count = sb->count + m;
	if (new_count > sb->capacity) {
		sb->items = realloc(sb->items, new_count);
		assert(sb->items != NULL);
		sb->capacity = new_count;
	}

	fread(sb->items + sb->count, m, 1, f);
	if (ferror(f)) { result = false; goto defer; }
	sb->count = new_count;

defer:
	if (!result) {
		printf("Could not read file %s: %s\n", filepath_cstr, strerror(errno));
	}
	if (f) { fclose(f); }
	return result;
}

bool write_to_file(const char* filepath_cstr, StringBuilder* sb) {
	FILE* f = fopen(filepath_cstr, "wb");
	if (f == NULL) {
		printf("Could not open file for writing: %s\n", strerror(errno));
		return false;
	}

	size_t written = fwrite(sb->items, 1, sb->count, f);
	if (written != sb->count) {
		printf("Error writing to file: %s\n", strerror(errno));
		fclose(f);
		return false;
	}

	fclose(f);
	return true;
}

typedef struct {
	StringBuilder* out;
	const char* cursor;
	bool in_paragraph;
	bool in_list;
} MdRenderer;

void da_append_escape_html(StringBuilder* out, const char* in, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		switch ((unsigned char)in[i]) {
			case '<':  da_append_cstr(out, "&lt;");   break;
			case '>':  da_append_cstr(out, "&gt;");   break;
			case '&':  da_append_cstr(out, "&amp;");  break;
			case '\'': da_append_cstr(out, "&#39;");  break;
			case '"':  da_append_cstr(out, "&quot;"); break;
			default:   da_append(out, in[i]);         break;
		}
	}
}

const char* search_str_until_newline(const char* haystack, const char* needle) {
	if (!haystack) return NULL;
	if (!*needle) return haystack;

	const char* hay = haystack;
	const char* ndl = needle;

	while (*hay && *hay != '\n') {
		const char* h_sub = hay;
		const char* n_sub = ndl;

		while (*n_sub) {
			if (*h_sub == '\n' || *h_sub == '\0') break;
			if (*h_sub != *n_sub) break;
			h_sub++;
			n_sub++;
		}
		if (*n_sub == '\0') return hay;

		hay++;
	}

	return NULL;
}

size_t sv_strstr(StringView haystack, StringView needle) {
	if (needle.count == 0) return 0;
	if (haystack.count < needle.count) return haystack.count;

	for (size_t i = 0; i <= haystack.count - needle.count; ++i) {
		if (memcmp(haystack.items + i, needle.items, needle.count) == 0) {
			return i;
		}
	}
	return haystack.count;
}

bool starts_with(const char* line, const char* prefix) {
	return strncmp(line, prefix, strlen(prefix)) == 0;
}
bool word_starts_with(const char* line, const char* prefix) {
	size_t len = strlen(prefix);
	return strncmp(line, prefix, len) == 0 && (*(line+len) != ' ');
}
const char* word_ends_with(const char* line, const char* prefix) {
	const char* end = search_str_until_newline(line, prefix);
	if (end && (*(end-1) == ' ')) return NULL;
	return end;
}

void parse_inline(MdRenderer* r, const char* line) {

#define PARSE_INLINE_TAG(start, end, html_start, html_end)             \
	else if (word_starts_with(p, start)) {                             \
		size_t start_len = sizeof(start) - 1;                          \
		size_t end_len   = sizeof(end) - 1;                            \
		const char* tag_end = word_ends_with(p + start_len, end);      \
		if (tag_end) {                                                 \
			p += start_len;                                            \
			da_append_cstr(r->out, html_start);                        \
			da_append_escape_html(r->out, p, tag_end - p);             \
			da_append_cstr(r->out, html_end);                          \
			p = tag_end + end_len;                                     \
			continue;                                                  \
		}                                                              \
	}

	const char* p = line;
	while (*p && *p != '\n') {
		// double space line break
		if (p[0] == ' ' && p[1] == ' ' && p[2] == '\n') {
			da_append_cstr(r->out, "<br>\n");
			p += 3;
			break;
		}

		if (false) {}
		PARSE_INLINE_TAG("***", "***", "<strong><i>", "</i></strong>")
		PARSE_INLINE_TAG("**_", "_**", "<strong><i>", "</i></strong>")
		PARSE_INLINE_TAG("_**", "**_", "<strong><i>", "</i></strong>")
		PARSE_INLINE_TAG("**", "**", "<strong>", "</strong>")
		PARSE_INLINE_TAG("*", "*", "<i>", "</i>")
		PARSE_INLINE_TAG("_", "_", "<i>", "</i>")
		PARSE_INLINE_TAG("`", "`", "<code>", "</code>")
		PARSE_INLINE_TAG("\\(", "\\)", "\\(", "\\)")
		else if (*p == '[') {
			const char* end_text = search_str_until_newline(p, "]");
			if (!end_text || end_text[1] != '(') {
				da_append_escape_html(r->out, p, 1);
				++p;
				continue;
			}
			const char* end_url = search_str_until_newline(end_text + 2, ")");
			if (!end_url) break;
			da_append_cstr(r->out, "<a href=\"");
			da_append_many(r->out, end_text + 2, end_url - (end_text + 2));
			da_append_cstr(r->out, "\">");
			da_append_escape_html(r->out, p + 1, end_text - (p + 1));
			da_append_cstr(r->out, "</a>");
			p = end_url + 1;
			continue;
		}
		else if (starts_with(p, "<?")) {
			const char* tag_end = strstr(p + 2, "?>");
			if (tag_end) {
				da_append_many(r->out, p, tag_end - p + 2);
				p = tag_end + 2;
				r->cursor = p;
				continue;
			}
		}

		da_append(r->out, *p);
		++p;
	}
}

// includes newline
void append_until_newline(StringBuilder* sb, const char* str) {
	size_t count = 0;
	const char* cursor = str;
	while (*cursor && *cursor != '\n') {
		count++;
		cursor++;
	}
	da_append_many(sb, str, count + 1);
}
void skip_after_newline(const char** cursor) {
	while (**cursor && **cursor != '\n') {
		(*cursor)++;
	}
	if (**cursor == '\n') {
		(*cursor)++;
	}
}

void render_md_to_html(StringBuilder* md, StringBuilder* out) {
	MdRenderer r = { .cursor = md->items, .out = out };

#define start_paragraph() if (!r.in_paragraph) { da_append_cstr(out,  "\n<p>\n"); r.in_paragraph = true; }
#define   end_paragraph() if (r.in_paragraph)  { da_append_cstr(out, "</p>\n"); r.in_paragraph = false; }
#define start_list() if (!r.in_list) { da_append_cstr(out,  "<ul>\n"); r.in_list = true; }
#define   end_list() if (r.in_list)  { da_append_cstr(out, "</ul>\n"); r.in_list = false; }

	while (*r.cursor) {
		const char* line_end = r.cursor;
		while (*line_end && *line_end != '\n') line_end++;

		const char* trimmed = r.cursor;
		while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

		// empty line ends paragraph
		if (line_end - trimmed == 0) {
			end_paragraph();
			end_list();

		} else if (starts_with(trimmed, "<?")) {
			const char* end = strstr(trimmed + 2, "?>");
			if (end) {
				da_append_many(out, trimmed, end - trimmed + 2);
				r.cursor = end + 2;
				continue;
			}
		
		} else if (starts_with(trimmed, "---\n")) {
			if (trimmed != md->items) {
				da_append_cstr(out, "<hr>");
				r.cursor = trimmed + 3;
				continue;
			}

			/* frontmatter
			const char* end = strstr(trimmed + 4, "---\n");
			if (end) {
				trimmed += 3;
				da_append_cstr(out_fm, "<?");
				da_append_many(out_fm, trimmed, end - trimmed);
				da_append_cstr(out_fm, "?>\n");
				r.cursor = end + 3;
				continue;
			}
			*/

		// HTML passthrough
		} else if (*trimmed == '<') {
			end_paragraph();
			end_list();

			const char* html_end_start = search_str_until_newline(trimmed, "</");
			const char* html_end_end   = search_str_until_newline(html_end_start, ">");
			if (!html_end_start || !html_end_end) {
				append_until_newline(out, trimmed);
			} else {
				html_end_end++;
				da_append_many(out, trimmed, html_end_end - trimmed);
				r.cursor = html_end_end;
				parse_inline(&r, html_end_end);
			}

		} else if (*trimmed == '#') {
			end_paragraph();
			end_list();

			int level = 0;
			while (*trimmed == '#') { level++; trimmed++; }
			while (*trimmed == ' ') trimmed++;

			char tag[16];
			sprintf(tag, "h%d", level);
			da_append_cstr(out, "\n<"); da_append_cstr(out, tag); da_append(out, '>');
			parse_inline(&r, trimmed);
			da_append_cstr(out, "</"); da_append_cstr(out, tag); da_append_cstr(out, ">\n");

		} else if (starts_with(trimmed, "- [ ] ")) {
			end_paragraph();
			end_list();
			da_append_cstr(out, "<ul><li><input type=\"checkbox\" disabled>");
			parse_inline(&r, trimmed + 6);
			da_append_cstr(out, "</li></ul>\n");

		} else if (starts_with(trimmed, "- ")) {
			end_paragraph();
			start_list();
			da_append_cstr(out, "<li>");
			parse_inline(&r, trimmed + 2);
			da_append_cstr(out, "</li>\n");

		} else if (starts_with(trimmed, "> ")) {
			end_paragraph();
			end_list();
			da_append_cstr(out, "<blockquote>");
			parse_inline(&r, trimmed + 2);
			da_append_cstr(out, "</blockquote>\n");

		} else if (starts_with(trimmed, "```")) {
			if (trimmed == md->items) {
				/* frontmatter
				const char* end = strstr(trimmed + 4, "```\n");
				if (end) {
					skip_after_newline(&trimmed);
					da_append_cstr(out_fm, "<?");
					da_append_many(out_fm, trimmed, end - trimmed);
					da_append_cstr(out_fm, "?>\n");
					r.cursor = end + 3;
					continue;
				}
				*/
			}

			end_paragraph();
			end_list();

			const char* code_end = strstr(trimmed + 3, "```");
			if (!code_end) code_end = md->items + md->count;

			skip_after_newline(&trimmed); // skip language
			da_append_cstr(out, "<pre><code>\n");
			da_append_escape_html(out, trimmed, code_end - trimmed);
			da_append_cstr(out, "</code></pre>\n");

			r.cursor = code_end + 3;
			continue;

		} else {
			end_list();
			start_paragraph();
			parse_inline(&r, trimmed);
			da_append(out, '\n');
		}

		if (r.cursor > line_end) continue;
		r.cursor = (*line_end == '\n') ? line_end + 1 : line_end;
	}

	end_paragraph();
	end_list();
	da_append(out, '\0');

#undef start_paragraph
#undef   end_paragraph
#undef start_list
#undef   end_list
}




void print_usage(const char* prog_name) {
	fprintf(stderr, "Usage: %s input.md [-o output.html]\n", prog_name);
}

int main(int argc, char** argv) {
	const char* input_path = NULL;
	const char* output_path = NULL;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			if (i + 1 < argc) {
				output_path = argv[++i];
			} else {
				return 1;
			}
		} else if (!input_path) {
			input_path = argv[i];
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			print_usage(argv[0]);
			return 1;
		}
	}

	if (!input_path) {
		print_usage(argv[0]);
		return 1;
	}

	StringBuilder source = {0};
	if (!read_entire_file(input_path, &source)) return 1;

	StringBuilder out = {0};
	render_md_to_html(&source, &out);
	da_append(&out, '\0');

	if (output_path) {
		write_to_file(output_path, &out);
	} else {
		puts(out.items);
	}

	free(source.items);
	free(out.items);
	return 0;
}
