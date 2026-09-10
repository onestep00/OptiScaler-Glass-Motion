// Experimental coarse connected regions for static transparent surface motion.
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> PreviousColor : register(t1);
Texture2D<float4> EngineMotion : register(t2);
Texture2D<float> EngineDepth : register(t3);
Texture2D<float> SurfaceDepth : register(t4);
Texture2D<float> PreviousSurfaceDepth : register(t5);
Texture2D<float> SeedSelection : register(t6);
RWTexture2D<float4> MotionOutput : register(u0);
RWTexture2D<float> DepthOutput : register(u1);
struct Cell { uint mask; uint seeds; uint evidence; uint baseError; uint surfaceError; uint separated; };
RWStructuredBuffer<Cell> Cells : register(u2);
globallycoherent RWStructuredBuffer<uint> Parents : register(u3);
struct Region { uint area; uint seeds; uint evidence; uint baseError; uint surfaceError; uint separated; };
RWStructuredBuffer<Region> Regions : register(u4);
RWStructuredBuffer<uint> Failures : register(u5);
RWTexture2D<float> SelectionOutput : register(u6);
SamplerState LinearClamp : register(s0);
cbuffer Parameters : register(b0) {
    uint2 MotionSize; float2 CurrentJitter;
    float2 MotionToUV; float2 InverseColorSize;
    uint HistoryValid; uint IdentityOnly; uint ValidateColor; uint Padding;
    row_major float4x4 ClipToPreviousClip;
};
uint2 gridSize(){return (MotionSize+3)/4;}
uint cellIndex(uint2 p){return p.y*gridSize().x+p.x;}
bool foreground(uint2 p,out float z){
    z=SurfaceDepth.Load(int3(p,0));float b=EngineDepth.Load(int3(p,0));
    return isfinite(z)&&z>0&&z>b+max(b*.002,1e-7);
}
bool geometry(uint2 p,float z,out float2 s,out float expected){
    float2 uv=(float2(p)+.5)/MotionSize;
    float2 u=uv-CurrentJitter/MotionSize;
    float4 q=mul(float4(u*float2(2,-2)+float2(-1,1),z,1),ClipToPreviousClip);
    s=0;expected=0;
    if(!all(isfinite(q))||q.w<=1e-6)return false;
    s=q.xy/q.w*float2(.5,-.5)+.5-u;expected=q.z/q.w;
    return all(isfinite(s))&&all(uv+s>0)&&all(uv+s<1);
}
bool reliable(uint2 p,float2 s,float expected,float2 b){
    float2 uv=(float2(p)+.5)/MotionSize;
    if(!all(isfinite(b))||length((s-b)*MotionSize)<.5||any(uv+b<2*InverseColorSize)||any(uv+b>1-2*InverseColorSize)||any(uv+s<2*InverseColorSize)||any(uv+s>1-2*InverseColorSize))return false;
    int2 at=int2((uv+s)*MotionSize);float error=1e10;
    static const int2 d[5]={int2(0,0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
    [unroll]for(uint i=0;i<5;i++){
        float z=PreviousSurfaceDepth.Load(int3(clamp(at+d[i],0,int2(MotionSize)-1),0));
        if(z>0)error=min(error,abs(z-expected));
    }
    return error<=max(abs(expected)*.015,1e-6);
}
[numthreads(8,8,1)]
void BuildCells(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=gridSize()))return;
    uint index=cellIndex(id.xy);Cell cell=(Cell)0;Regions[index]=(Region)0;
    Parents[index]=index;if(index==0)Failures[0]=0;
    if(HistoryValid&&!IdentityOnly){
        [unroll]for(uint y=0;y<4;y++)[unroll]for(uint x=0;x<4;x++){
            uint2 p=id.xy*4+uint2(x,y);float z;
            if(any(p>=MotionSize)||!foreground(p,z))continue;
            cell.mask|=1u<<(y*4+x);
            cell.separated+=(z-EngineDepth.Load(int3(p,0)))>z*.02f?1u:0u;
            cell.seeds+=SeedSelection.Load(int3(p,0))>0?1:0;
            float2 s;float expected;
            if(!geometry(p,z,s,expected))continue;
            float2 b=EngineMotion.Load(int3(p,0)).xy*MotionToUV;
            if(!reliable(p,s,expected,b))continue;
            float2 uv=(float2(p)+.5)/MotionSize;
            float3 c=CurrentColor.SampleLevel(LinearClamp,uv,0).rgb;
            float eb=dot(abs(PreviousColor.SampleLevel(LinearClamp,uv+b,0).rgb-c),float3(85,85,85));
            float es=dot(abs(PreviousColor.SampleLevel(LinearClamp,uv+s,0).rgb-c),float3(85,85,85));
            cell.evidence++;cell.baseError+=(uint)round(eb);cell.surfaceError+=(uint)round(es);
        }
    }
    Cells[index]=cell;
}
uint findRoot(uint index){
    [loop]for(uint n=0;n<256;n++){
        uint parent;InterlockedAdd(Parents[index],0,parent);
        if(parent==index)return index;
        uint grand;InterlockedAdd(Parents[parent],0,grand);
        uint unused;InterlockedMin(Parents[index],grand,unused);index=parent;
    }
    uint unused;InterlockedAdd(Failures[0],1,unused);return 0xffffffff;
}
void join(uint a,uint b){
    [loop]for(uint n=0;n<128;n++){
        a=findRoot(a);b=findRoot(b);if(a==b||a==0xffffffff||b==0xffffffff)return;
        uint hi=max(a,b),lo=min(a,b),old;InterlockedMin(Parents[hi],lo,old);
        if(old==hi)return;
    }
    uint unused;InterlockedAdd(Failures[0],1,unused);
}
[numthreads(8,8,1)]
void JoinCells(uint3 id:SV_DispatchThreadID){
    uint2 size=gridSize();if(any(id.xy>=size))return;
    uint index=cellIndex(id.xy),a=Cells[index].mask;if(a==0)return;
    if(id.x+1<size.x){uint b=Cells[index+1].mask;uint right=(a>>3)&0x1111,left=b&0x1111;if((right&(left|(left<<4)|(left>>4)))!=0)join(index,index+1);}
    if(id.y+1<size.y){
        uint b=Cells[index+size.x].mask;uint bottom=(a>>12)&15,top=b&15;if((bottom&(top|(top<<1)|(top>>1)))!=0)join(index,index+size.x);
        if(id.x+1<size.x&&(a&0x8000)!=0&&(Cells[index+size.x+1].mask&1)!=0)join(index,index+size.x+1);
        if(id.x>0&&(a&0x1000)!=0&&(Cells[index+size.x-1].mask&8)!=0)join(index,index+size.x-1);
    }
}
[numthreads(8,8,1)]
void AccumulateRegions(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=gridSize()))return;uint index=cellIndex(id.xy);Cell c=Cells[index];if(c.mask==0)return;
    uint root=findRoot(index);if(root==0xffffffff)return;Parents[index]=root;
    InterlockedAdd(Regions[root].area,countbits(c.mask));InterlockedAdd(Regions[root].seeds,c.seeds);
    InterlockedAdd(Regions[root].separated,c.separated);
    InterlockedAdd(Regions[root].evidence,c.evidence);InterlockedAdd(Regions[root].baseError,c.baseError);InterlockedAdd(Regions[root].surfaceError,c.surfaceError);
}
[numthreads(8,8,1)]
void ApplyRegions(uint3 id:SV_DispatchThreadID){
    uint2 p=id.xy;if(any(p>=MotionSize))return;
    float4 mv=EngineMotion.Load(int3(p,0));float depth=EngineDepth.Load(int3(p,0));float selected=0;
    float surface;
    if(HistoryValid&&!IdentityOnly&&Failures[0]==0&&foreground(p,surface)){
        uint root=Parents[cellIndex(p/4)];Region r=Regions[root];
        bool accepted=float(r.separated)>=float(r.area)*.2f&&r.area>=80u&&r.seeds>=16u&&float(r.seeds)>=float(r.area)*.08f&&r.evidence>=32u&&float(r.evidence)>=float(r.area)*.2f&&r.baseError>r.evidence*3u&&float(r.surfaceError)<float(r.baseError)*1.02f+float(r.evidence);
        float2 s;float expected;
        if(accepted&&geometry(p,surface,s,expected)){mv.xy=s/MotionToUV;depth=surface;selected=1;}
    }
    MotionOutput[p]=mv;DepthOutput[p]=depth;SelectionOutput[p]=selected;
}
