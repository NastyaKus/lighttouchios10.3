// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal, bounded bplist00 edit for LaunchDaemons/<job>/Disabled=true.
// Preserve all existing object bodies (including opaque existing values) and object IDs;
// append copies of only the five changed dictionaries + shared key/true nodes.
#include "launchd-cache.h"
#ifndef __APPLE__
#include <stdlib.h>
#include <string.h>
#endif
#define LIMIT (32UL*1024*1024)
typedef unsigned long long U;
struct Plist { const unsigned char *b; size_t size; U count,top,table; unsigned os,rs; };
static const char *labels[]={"com.apple.voicemail.vmd","com.apple.CommCenter","com.apple.CommCenterMobileHelper","com.apple.CommCenterRootHelper","com.apple.locationd"};
static U be(const unsigned char *b,unsigned n) { U x=0;while(n--)x=(x<<8)|*b++;return x; }
static void put(unsigned char *b,unsigned n,U x) { while(n){b[--n]=(unsigned char)x;x>>=8;} }
static int width(unsigned n) {return n==1 || n==2 || n==4 || n==8;}
static unsigned bytes_for(U n) {return n<256?1:n<65536?2:n<0x100000000ULL?4:8;}
static int count_at(const struct Plist *p,U off,U *n,U *data) {
 if(off>=p->table)return 0;
 unsigned low=p->b[off]&15;
 if(low<15){*n=low;*data=off+1;return 1;}
 if(off+2>p->table || (p->b[off+1]>>4)!=1 || (p->b[off+1]&15)>3)return 0;
 unsigned w=1U<<(p->b[off+1]&15);
 if(w>p->table-off-2)return 0;
 *n=be(p->b+off+2,w);*data=off+2+w;return 1;
}
static int object_at(const struct Plist *p,U id,U *off) {
 if(id>=p->count)return 0;
 *off=be(p->b+p->table+id*p->os,p->os);return *off>=8 && *off<p->table;
}
static int parse(const unsigned char *b,size_t size,struct Plist *p) {
 if(size<40 || size>LIMIT || memcmp(b,"bplist00",8)!=0)return 0;
 const unsigned char *t=b+size-32;
 p->b=b;p->size=size;p->os=t[6];p->rs=t[7];p->count=be(t+8,8);p->top=be(t+16,8);p->table=be(t+24,8);
 if(!width(p->os)||!width(p->rs)||!p->count||p->top>=p->count||p->table<8||p->table>size-32||p->count>(size-32-p->table)/p->os)return 0;
 // Validate lengths and every container reference, without recursive traversal.
 for(U id=0;id<p->count;id++) {
  U o,n=0,d=0,span=0;if(!object_at(p,id,&o))return 0;
  unsigned type=b[o]>>4,low=b[o]&15;
  if(type==0) {if(low!=0&&low!=8&&low!=9&&low!=15)return 0;span=1;}
  else if(type==1||type==2) {if(low>4 || (type==2&&low!=2&&low!=3))return 0;span=1+(1U<<low);}
  else if(type==3){if(low!=3)return 0;span=9;}
  else if(type==8){span=2+low;}
  else if(type==4||type==5||type==6||type==10||type==13) {
   if(!count_at(p,o,&n,&d))return 0;
   U unit=type==6?2:type>=10?p->rs:1;
   if(type==13)unit*=2;
   if(n>(p->table-d)/unit)return 0;
   span=d-o+n*unit;
   if(type>=10)for(U j=0;j<n*(type==13?2:1);j++)if(be(b+d+j*p->rs,p->rs)>=p->count)return 0;
  } else return 0;
  if(span>p->table-o)return 0;
  if(type==5)for(U k=0;k<n;k++)if(b[d+k]>=128)return 0;
  if(type==6)for(U k=0;k<n;k++) {
   U c=be(b+d+2*k,2);
   if(c>=0xd800 && c<=0xdbff){if(++k>=n)return 0;U low=be(b+d+2*k,2);if(low<0xdc00||low>0xdfff)return 0;}
   else if(c>=0xdc00&&c<=0xdfff)return 0;
  }
  if(type==13)for(U k=0;k<n;k++){U key;if(!object_at(p,be(b+d+k*p->rs,p->rs),&key))return 0;unsigned kt=b[key]>>4;if(kt!=5&&kt!=6)return 0;}
  if(type==3){union {U u;double d;} date;date.u=be(b+o+1,8);if(!(date.d>=-63113904000.0&&date.d<252423993600.0))return 0;}
 }
 return 1;
}
static int equal_string(const struct Plist *p,U id,const char *text) {
 U o,n,d;if(!object_at(p,id,&o)||!count_at(p,o,&n,&d))return 0;
 size_t len=strlen(text);if(n!=len)return 0;
 if((p->b[o]>>4)==5)return memcmp(p->b+d,text,len)==0;
 if((p->b[o]>>4)!=6)return 0;
 for(size_t i=0;i<len;i++)if(p->b[d+2*i]!=0||p->b[d+2*i+1]!=(unsigned char)text[i])return 0;
 return 1;
}
static int dict(const struct Plist *p,U id,U *n,U *d) {
 U o;return object_at(p,id,&o)&&(p->b[o]>>4)==13&&count_at(p,o,n,d);
}
// Return 1 found, 0 missing, -1 malformed/duplicate.
static int lookup(const struct Plist *p,U id,const char *key,U *value,U *slot) {
 U n,d;if(!dict(p,id,&n,&d))return -1;int found=0;
 for(U i=0;i<n;i++)if(equal_string(p,be(p->b+d+i*p->rs,p->rs),key)) {
  if(found)return -1;
  found=1;*slot=d+(n+i)*p->rs;*value=be(p->b+*slot,p->rs);
 }
 return found;
}
struct Targets {U id[5],slot[5],dn[5],dd[5],disabled_index[5];int all;};
static int targets(const struct Plist *p,struct Targets *t,const char **why) {
 U jobs,unused,n,d;if(lookup(p,p->top,"LaunchDaemons",&jobs,&unused)!=1||!dict(p,jobs,&n,&d)){*why="missing LaunchDaemons dictionary";return 0;}
 int seen[5]={0},springboard=0;t->all=1;
 for(U j=0;j<n;j++) {
  U id=be(p->b+d+(n+j)*p->rs,p->rs),label,slot,nj,dj;
  if(!dict(p,id,&nj,&dj))continue;
  int got=lookup(p,id,"Label",&label,&slot);if(got<0){*why="duplicate Label";return 0;}if(!got)continue;
  if(equal_string(p,label,"com.apple.SpringBoard"))springboard++;
  for(unsigned i=0;i<5;i++)if(equal_string(p,label,labels[i])) {
   if(seen[i]++){*why="duplicate target service";return 0;}
   t->id[i]=id;t->slot[i]=d+(n+j)*p->rs;t->dn[i]=nj;t->dd[i]=dj;t->disabled_index[i]=~0ULL;
   U val=0,vs=0;int has=lookup(p,id,"Disabled",&val,&vs);
   if(has<0){*why="duplicate Disabled key";return 0;}
   if(has){U o;if(!object_at(p,val,&o)||(p->b[o]!=8&&p->b[o]!=9)){*why="Disabled is not a boolean";return 0;}
    t->disabled_index[i]=(vs-dj)/p->rs-nj;if(p->b[o]!=9)t->all=0;
   }else t->all=0;
  }
 }
 if(springboard!=1){*why="missing/duplicate SpringBoard safety guard";return 0;}
 for(unsigned i=0;i<5;i++)if(seen[i]!=1){*why="missing target service";return 0;}
 return 1;
}
int lc_verify(const unsigned char *b,size_t n,const char **why) {
 struct Plist p;struct Targets t;
 if(!parse(b,n,&p)){*why="invalid/unsupported binary plist";return 0;}
 if(!targets(&p,&t,why))return 0;
 if(!t.all){*why="five services are not all Disabled=true";return 0;}
 return 1;
}
static unsigned count_bytes(U n) {return n<15?1:2+bytes_for(n);}
static unsigned emit_count(unsigned char *out,U n) {
 if(n<15){out[0]=(unsigned char)(0xd0|n);return 1;}
 unsigned w=bytes_for(n),exp=w==1?0:w==2?1:w==4?2:3;out[0]=0xdf;out[1]=(unsigned char)(0x10|exp);put(out+2,w,n);return 2+w;
}
int lc_patch(const unsigned char *b,size_t n,unsigned char **out,size_t *outn,const char **why) {
 *out=NULL;*outn=0;struct Plist p;struct Targets t;
 if(!parse(b,n,&p)){*why="invalid/unsupported binary plist";return 0;}
 if(!targets(&p,&t,why))return 0;
 if(t.all){unsigned char *same=malloc(n);if(!same){*why="allocation failed";return 0;}memcpy(same,b,n);*out=same;*outn=n;return 1;}
 U nc=p.count+7;
 if(nc<p.count||(p.rs<8 && nc-1>= (1ULL<<(8*p.rs)))){*why="object reference width exhausted";return 0;}
 U pos=p.table;
 for(unsigned i=0;i<5;i++){U nn=t.dn[i]+(t.disabled_index[i]==~0ULL);if(nn>LIMIT/(2*p.rs)){*why="dictionary too large";return 0;}pos+=count_bytes(nn)+nn*2*p.rs;}
 pos+=10; // ASCII Disabled (9) + true (1).
 if(pos>LIMIT-32 || nc>(LIMIT-pos-32)/8){*why="output too large";return 0;}
 unsigned os=bytes_for(pos);U total=pos+nc*os+32;
 unsigned char *v=malloc((size_t)total);U *offsets=malloc((size_t)nc*sizeof(U));
 if(!v||!offsets){free(v);free(offsets);*why="allocation failed";return 0;}
 memcpy(v,b,(size_t)p.table);
 for(U i=0;i<p.count;i++)object_at(&p,i,&offsets[i]);
 U at=p.table;
 for(unsigned i=0;i<5;i++) {
  offsets[p.count+i]=at;U old=t.dn[i],nn=old+(t.disabled_index[i]==~0ULL);
  at+=emit_count(v+at,nn);U key=at,val=at+nn*p.rs;
  memcpy(v+key,b+t.dd[i],(size_t)old*p.rs);memcpy(v+val,b+t.dd[i]+old*p.rs,(size_t)old*p.rs);
  if(nn>old){put(v+key+old*p.rs,p.rs,p.count+5);put(v+val+old*p.rs,p.rs,p.count+6);}
  else put(v+val+t.disabled_index[i]*p.rs,p.rs,p.count+6);
  at+=nn*2*p.rs;put(v+t.slot[i],p.rs,p.count+i);
 }
 offsets[p.count+5]=at;v[at++]=0x58;memcpy(v+at,"Disabled",8);at+=8;
 offsets[p.count+6]=at;v[at++]=9;
 if(at!=pos){free(v);free(offsets);*why="internal size mismatch";return 0;}
 for(U i=0;i<nc;i++){put(v+at,os,offsets[i]);at+=os;}
 memset(v+at,0,32);v[at+6]=(unsigned char)os;v[at+7]=(unsigned char)p.rs;put(v+at+8,8,nc);put(v+at+16,8,p.top);put(v+at+24,8,pos);
 free(offsets);
 if(!lc_verify(v,(size_t)total,why)){free(v);return 0;}
 *out=v;*outn=(size_t)total;return 1;
}
