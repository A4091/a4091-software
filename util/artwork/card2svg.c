#include <stdio.h>
#include <stdlib.h>

// Type definitions to match Amiga types roughly
typedef char BYTE;
typedef short SHORT;

struct drawing {
    BYTE type; // 1: rectangle filled, 2: rectangle empty, 3: zorro pins, -1: end
    BYTE pen;  // Color index
    SHORT x, y, w, h;
};

#include "examples/a4091.h"
#include "examples/a4092.h"

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))

// Map Amiga "Pens" to SVG Colors
// Pen 0: Background/Cutout
// Pen 1: Black (Chips)
// Pen 2: White/Silver (Silkscreen/Metal)
// Pen 3: Board Color (Red for A4092)
const char *get_color(int pen) {
    switch(pen) {
        case 0: return "#b2b2b2"; // Dark Grey hole
        case 1: return "#000000"; // Black chips
        case 2: return "#ffffff"; // Silver/White metal/silk
        //case 3: return "#CC4444"; // Red PCB (A4092)
        case 3: return "#0055aa"; // Red PCB (A4092)
        default: return "#FF00FF";
    }
}

void write_svg(const char *filename, const struct drawing *data, int count) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        perror("Error opening file");
        return;
    }

    // Canvas size (approximate based on bootmenu offsets)
    int width = 640;
    int height = 400;
    int offset_x = 103;
    int offset_y = 50;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n");
    fprintf(f, "<svg width=\"%d\" height=\"%d\" xmlns=\"http://www.w3.org/2000/svg\">\n", width, height);
    
    // Background
    fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#b2b2b2\" />\n");
    
    // Group with offset to match bootmenu placement
    fprintf(f, "<g transform=\"translate(%d, %d)\">\n", offset_x, offset_y);

    for (int i = 0; i < count; i++) {
        struct drawing d = data[i];
        
        // --- ASPECT RATIO LOGIC FROM BOOTMENU.C ---
        // SHORT x1=x+d.x, y1=y+d.y/2, x2=x+d.x+d.w, y2=y+d.y/2+d.h/2;
        int x = d.x;
        int y = d.y;// / 2;
        int w = d.w;
        int h = d.h;// / 2;

        const char *color = get_color(d.pen);

        if (d.type == 1) { 
            // Type 1: Filled Rectangle
            fprintf(f, "  <rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"%s\" />\n",
                    x, y, w, h, color);
        } 
        else if (d.type == 2) { 
            // Type 2: Empty Rectangle (Outline)
            // bootmenu.c draws 4 lines. We use a rect with stroke.
            fprintf(f, "  <rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"%s\" stroke-width=\"1\" />\n",
                    x, y, w, h, color);
        } 
        else if (d.type == 3) { 
            // Type 3: Vertical Stripes (Zorro pins)
            // Logic: for(j=x1; j<x2; j+=4) RectFill(rp, j, y1, j+1, y2);
            fprintf(f, "  <g fill=\"%s\">\n", color);
            for (int j = x; j < x + w; j += 4) {
                fprintf(f, "    <rect x=\"%d\" y=\"%d\" width=\"1\" height=\"%d\" />\n", j, y, h);
            }
            fprintf(f, "  </g>\n");
        }
    }

    fprintf(f, "</g>\n");
    fprintf(f, "</svg>\n");
    fclose(f);
    printf("Generated %s\n", filename);
}

int main() {
    write_svg("examples/a4091.svg", card_a4091, ARRAY_LENGTH(card_a4091));
    write_svg("examples/a4092.svg", card_a4092, ARRAY_LENGTH(card_a4092));
    return 0;
}
