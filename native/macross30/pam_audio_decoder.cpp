/* Independently implemented bounded PAMF private00/ATS demuxer.
 * Format references: RPCS3 cellAtracXdec.h ATS field description and original
 * title byte inventory. No RPCS3 implementation copied. */
#include "pam_audio_decoder.h"
#include "../../vendor/ps3recomp-master/third_party/at3_standalone/at3_bridge.h"
#include <fstream>
#include <filesystem>
#include <vector>
#include <deque>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <memory>
#include <stdexcept>
struct MacrossPamAudioDecoder {
 std::ifstream file; uint64_t end=0, elementary=0, consumed=0;
 std::vector<unsigned char> pending; std::vector<float> pcm;
 struct Mark{uint64_t offset;int64_t pts;};std::deque<Mark> marks;
 void* codec=nullptr; std::string error; bool eof=false; uint32_t index=0;
 int64_t anchor=0;uint64_t anchor_frame=0;bool anchored=false;
 ~MacrossPamAudioDecoder(){if(codec)at3p_close(codec);}
 void fail(const char*s){throw std::runtime_error(s);}
 void read(void*p,size_t n){if(n&&!file.read(static_cast<char*>(p),n))fail("Truncated PAMF packet");}
 uint64_t position(){auto p=file.tellg();if(p<0)fail("Invalid file position");return static_cast<uint64_t>(p);}
 void skip(uint64_t n){auto p=position();if(n>end-p)fail("Packet extends beyond file");file.seekg(static_cast<std::streamoff>(n),std::ios::cur);if(!file)fail("Seek failed");}
 bool packet(){
  while(position()<end){
   unsigned char h[6];read(h,4);if(h[0]||h[1]||h[2]!=1)fail("Invalid MPEG-PS packet boundary");
   if(h[3]==0xb9){if(position()!=end)fail("Bytes after MPEG end code");eof=true;return false;}
   if(h[3]==0xba){unsigned char p[10];read(p,10);if((p[0]&0xc0)!=0x40)fail("Unsupported pack header");skip(p[9]&7);continue;}
   if(h[3]<0xbb||h[3]==0xff)fail("Unsupported MPEG packet code");
   read(h+4,2);size_t n=(h[4]<<8)|h[5];if(!n)fail("Unbounded PES unsupported");
   if(h[3]!=0xbd){skip(n);continue;}
   std::vector<unsigned char>b(n);read(b.data(),n);
   if(n<3||(b[0]&0xc0)!=0x80)fail("Invalid private PES header");
   size_t start=3+b[2];if(start+4>n)fail("Missing private stream header");
   if(b[start]!=0)continue; // Other sound streams are not silently interpreted.
   int flags=(b[1]>>6)&3;int64_t pts=0;bool valid=false;
   if(flags==1)fail("Invalid PES timestamp flags");
   if(flags>=2){if(b[2]<5)fail("Truncated PTS");auto*p=b.data()+3;
    if(!(p[0]&1)||!(p[2]&1)||!(p[4]&1)||(p[0]>>4)!=flags)fail("Malformed PTS");
    pts=(int64_t((p[0]>>1)&7)<<30)|(int64_t(p[1])<<22)|(int64_t(p[2]>>1)<<15)|(int64_t(p[3])<<7)|(p[4]>>1);valid=true;}
   uint32_t next=(uint32_t(b[start+1])<<16)|(b[start+2]<<8)|b[start+3];
   size_t bytes=n-start-4;if(!bytes)fail("Empty selected audio PES");
   // Original title streams use FFFF in no-PTS continuation PES packets:
   // there is no newly timestamped AU start in this packet. The offset is
   // only consumed by the valid-PTS mark below. Preserve ALL payload bytes
   // (including final-AU continuation); dropping them truncates real audio.
   // Timestamped FFFF and every other out-of-range offset still fail closed.
   if(next>bytes&&!(next==0xffff&&!valid))fail("Private AU offset beyond PES");
   if(valid){if(marks.size()>=128)fail("Excess pending audio timestamps");marks.push_back({elementary+next,pts});}
   pending.insert(pending.end(),b.begin()+start+4,b.end());elementary+=bytes;
   if(pending.size()>65535+696)fail("Audio carry exceeds bound");return true;
  }eof=true;return false;
 }
};
extern "C" MacrossPamAudioDecoder* macross_pam_audio_open(const char*path,char*error,size_t cap){
 try{if(!path)throw std::runtime_error("Null path");auto d=std::make_unique<MacrossPamAudioDecoder>();d->file.open(std::filesystem::u8path(path),std::ios::binary);
 if(!d->file)throw std::runtime_error("Cannot open PAMF");d->file.seekg(0,std::ios::end);auto end=d->file.tellg();if(end<16)throw std::runtime_error("Short PAMF");d->end=uint64_t(end);d->file.seekg(0);
 unsigned char h[16];d->read(h,16);if(memcmp(h,"PAMF",4))throw std::runtime_error("Invalid PAMF signature");
 uint64_t off=((uint64_t(h[8])<<24)|(uint64_t(h[9])<<16)|(uint64_t(h[10])<<8)|h[11])*2048;
 if(off<16||off>=d->end)throw std::runtime_error("Invalid PAMF payload offset");d->file.seekg(static_cast<std::streamoff>(off));return d.release();
 }catch(const std::exception&e){if(error&&cap)snprintf(error,cap,"%s",e.what());return nullptr;}}
extern "C" int macross_pam_audio_next_frame(MacrossPamAudioDecoder*d,MacrossPamAudioFrame*out){
 if(!d||!out)return -1;if(!d->error.empty())return -1;*out={};
 try{
 while(d->pending.size()<8&&!d->eof)d->packet();
 if(d->pending.empty()&&d->eof)return 0;if(d->pending.size()<8)d->fail("Truncated ATS header");
 auto*h=d->pending.data();if(h[0]!=0x0f||h[1]!=0xd0)d->fail("Invalid ATS sync");unsigned param=(h[2]<<8)|h[3];
 if(param!=0x4855||h[4]||h[5]||h[6]||h[7])d->fail("Unsupported ATS configuration");
 constexpr size_t bytes=688,au=696;while(d->pending.size()<au&&!d->eof)d->packet();if(d->pending.size()<au)d->fail("Truncated ATRAC access unit");
 while(!d->marks.empty()&&d->marks.front().offset<=d->consumed){auto m=d->marks.front();d->marks.pop_front();if(m.offset!=d->consumed)d->fail("PTS not aligned to audio access unit");d->anchor=m.pts;d->anchor_frame=d->index;d->anchored=true;}
 if(!d->codec){int ba=688;d->codec=at3p_open(2,&ba);if(!d->codec||ba!=688)d->fail("AT3P decoder creation failed");}
 float l[2048],r[2048];int samples=0;int rc=at3p_decode(d->codec,l,r,&samples,d->pending.data()+8,bytes);if(rc!=688||samples!=2048)d->fail("AT3P frame decode failed");
 d->pcm.resize(4096);for(int i=0;i<2048;i++){if(!std::isfinite(l[i])||!std::isfinite(r[i]))d->fail("Nonfinite decoded PCM");d->pcm[i*2]=l[i];d->pcm[i*2+1]=r[i];}
 out->interleaved=d->pcm.data();out->samples_per_channel=2048;out->channels=2;out->sample_rate=48000;out->frame_index=d->index;out->pts_valid=d->anchored;
 out->pts90k=d->anchor+int64_t(d->index-d->anchor_frame)*3840;d->index++;d->consumed+=au;d->pending.erase(d->pending.begin(),d->pending.begin()+au);return 1;
 }catch(const std::exception&e){d->error=e.what();return -1;}}
extern "C" const char* macross_pam_audio_last_error(const MacrossPamAudioDecoder*d){return d?d->error.c_str():"Null decoder";}
extern "C" void macross_pam_audio_close(MacrossPamAudioDecoder*d){delete d;}
