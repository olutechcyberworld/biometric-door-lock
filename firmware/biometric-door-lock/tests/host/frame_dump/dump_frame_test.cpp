// Host test: compiles the firmware's frame-dump code (who_bench.hpp) and prints one frame in the wire format,
// so tests/host/frame_dump/check_roundtrip.py can feed it to the dashboard's parser. Usage: dump_frame_test <mode 1|2|3|9> <big_endian 0|1>
#include "who_bench.hpp"
#include <cmath>
using namespace who::bench;
static uint16_t rgb565(int r,int g,int b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
int main(int argc,char**argv){
    int mode = argc>1?atoi(argv[1]):1; if(mode==9){heartbeat();return 0;}
    bool be = argc>2?atoi(argv[2]):1;
    const int W=240,H=240;
    static uint8_t buf[W*H*2];
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int r=x, g=y, b=128;                    // gradient background: red grows right, green grows down
        double dx=x-120, dy=y-110;
        if(dx*dx+dy*dy<50*50){ r=230; g=190; b=160; }       // "face"
        if(std::hypot(x-102,y-95)<7||std::hypot(x-138,y-95)<7){ r=20;g=20;b=20; } // eyes
        if(y>140 && y<148 && x>100 && x<140){ r=150; g=30; b=30; }                // mouth
        uint16_t v=rgb565(r,g,b);
        uint8_t hi=v>>8, lo=v&0xff;
        buf[(y*W+x)*2+ (be?0:1)] = hi; buf[(y*W+x)*2+(be?1:0)] = lo;
    }
    dl::image::img_t img; img.data=buf; img.width=W; img.height=H;
    img.pix_type = be? dl::image::DL_IMAGE_PIX_TYPE_RGB565BE : dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    std::list<dl::detect::result_t> res;
    dl::detect::result_t r; r.category=0; r.score=0.93f; r.box={68,50,172,170};
    r.keypoint={102,95,138,95,120,118,106,145,134,145}; res.push_back(r);
    g_dump_req.store(mode);
    printf("I (1) noise: some log line before\n");
    dump_frame(img,res);
    printf("BENCH,0,1,40000,300000,600000,0,0.812\n");
}
