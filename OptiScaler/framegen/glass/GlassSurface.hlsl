// Experimental curved-surface candidate; offline FG quality validation required.
// Experimental FG input selector. Only measured surface/background evidence can
// select a surface vector. Auxiliary depth alone never selects a glass pane.
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> PreviousColor : register(t1);
Texture2D<float4> EngineMotion : register(t2);
Texture2D<float> EngineDepth : register(t3);
Texture2D<float> SurfaceDepth : register(t4);
Texture2D<float> PreviousSurfaceDepth : register(t5);
RWTexture2D<float4> MotionOutput : register(u0);
RWTexture2D<float> DepthOutput : register(u1);
RWTexture2D<float> SelectionOutput : register(u2);
SamplerState LinearClamp : register(s0);
cbuffer Parameters : register(b0) {
    uint2 MotionSize; float2 CurrentJitter;
    float2 MotionToUV; float2 InverseColorSize;
    uint HistoryValid; uint IdentityOnly; uint ValidateColor; uint Padding;
    row_major float4x4 ClipToPreviousClip;
};
groupshared uint votes[100];
bool inside(float2 uv) { return all(uv>2*InverseColorSize)&&all(uv<1-2*InverseColorSize); }
bool prepare(int2 pixel,out float2 candidate,out float surface,out float confidence) {
    candidate=0;surface=0;confidence=0;
    if(any(pixel<0)||any(pixel>=int2(MotionSize))||!HistoryValid||IdentityOnly)return false;
    surface=SurfaceDepth.Load(int3(pixel,0));
    float opaque=EngineDepth.Load(int3(pixel,0));
    if(!isfinite(surface)||surface<=0||surface<=opaque+max(opaque*.002,1e-7))return false;
    float2 uv=(float2(pixel)+.5)/MotionSize;
    float2 u=uv-CurrentJitter/MotionSize;
    float4 p=mul(float4(u*float2(2,-2)+float2(-1,1),surface,1),ClipToPreviousClip);
    if(!all(isfinite(p))||p.w<=1e-6)return false;
    candidate=p.xy/p.w*float2(.5,-.5)+.5-u;
    float2 base=EngineMotion.Load(int3(pixel,0)).xy*MotionToUV;
    if(!all(isfinite(base))||!all(isfinite(candidate))||!inside(uv+candidate)||!inside(uv+base))return false;
    if(length((candidate-base)*MotionSize)<.5)return false;
    int2 previousPixel=int2((uv+candidate)*MotionSize);
    float expected=p.z/p.w,depthError=1e10;
    static const int2 cross[5]={int2(0,0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
    [unroll]for(uint k=0;k<5;k++) {
        float z=PreviousSurfaceDepth.Load(int3(clamp(previousPixel+cross[k],0,int2(MotionSize)-1),0));
        if(z>0)depthError=min(depthError,abs(z-expected));
    }
    if(depthError>max(abs(expected)*.015,1e-6))return false;

    // A flat plane has affine inverse depth. Reject it before color selection.
    // Curvature is a geometry cue, not proof of material opacity.
    float curvature=0;
    [unroll]for(uint axis=0;axis<2;axis++) {
        int2 d=axis==0?int2(4,0):int2(0,4);
        float3 signedCurves=0;uint validTriples=0;
        [unroll]for(int shift=-1;shift<=1;shift++) {
            int2 p0=pixel+d*shift-d,p1=pixel+d*shift,p2=pixel+d*shift+d;
            if(any(p0<0)||any(p2<0)||any(p0>=int2(MotionSize))||any(p2>=int2(MotionSize)))continue;
            float z0=SurfaceDepth.Load(int3(p0,0)),z1=SurfaceDepth.Load(int3(p1,0)),z2=SurfaceDepth.Load(int3(p2,0));
            float b0=EngineDepth.Load(int3(p0,0)),b1=EngineDepth.Load(int3(p1,0)),b2=EngineDepth.Load(int3(p2,0));
            bool same=z0>b0+max(b0*.002,1e-7)&&z1>b1+max(b1*.002,1e-7)&&z2>b2+max(b2*.002,1e-7);
            if(same&&max(abs(z0-surface),max(abs(z1-surface),abs(z2-surface)))<surface*.02)
                {float c=(z0+z2-2*z1)/surface;if(shift==-1)signedCurves.x=c;else if(shift==0)signedCurves.y=c;else signedCurves.z=c;validTriples++;}
        }

        float3 magnitude=abs(signedCurves);
        float smallest=min(magnitude.x,min(magnitude.y,magnitude.z));
        float largest=max(magnitude.x,max(magnitude.y,magnitude.z));
        bool sameSign=all(signedCurves>0)||all(signedCurves<0);
        if(validTriples==3&&sameSign&&largest<smallest*8)curvature=max(curvature,smallest);
    }
    if(curvature<2e-5)return false;
    if(!ValidateColor){confidence=1;return true;}

    float eb=0,es=0,weight=0;
    // Average evidence over the same nearby surface to reduce isolated decisions.
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++) {
        int2 q=pixel+int2(x,y)*4;
        float z=SurfaceDepth.Load(int3(clamp(q,0,int2(MotionSize)-1),0));
        if(abs(z-surface)>surface*.02)continue;
        float2 uvq=uv+float2(x,y)*4/MotionSize;
        float3 c=CurrentColor.SampleLevel(LinearClamp,uvq,0).rgb;
        eb+=dot(abs(PreviousColor.SampleLevel(LinearClamp,uvq+base,0).rgb-c),float3(1,1,1)/3);
        es+=dot(abs(PreviousColor.SampleLevel(LinearClamp,uvq+candidate,0).rgb-c),float3(1,1,1)/3);
        weight+=1;
    }
    if(weight<5||eb/weight<3.0/255.0||es>eb*1.05)return false;
    confidence=max(.001,1-es/max(eb,1e-6));return true;

}
[numthreads(10,10,1)]
void ApplySurface(uint3 group:SV_GroupID,uint3 tid:SV_GroupThreadID,uint lane:SV_GroupIndex) {
    int2 pixel=int2(group.xy*8+tid.xy)-1;
    float2 candidate;float surface,confidence;
    bool selected=prepare(pixel,candidate,surface,confidence);
    votes[lane]=selected?1:0;
    GroupMemoryBarrierWithGroupSync();
    // The halo participates in the barrier and supplies neighboring votes.
    if(any(tid.xy<1)||any(tid.xy>8)||any(pixel>=int2(MotionSize)))return;
    uint support=0;
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)support+=votes[int(lane)+y*10+x];
    float4 mv=EngineMotion.Load(int3(pixel,0));
    float depth=EngineDepth.Load(int3(pixel,0));
    if(selected&&support>=5){mv.xy=candidate/MotionToUV;depth=surface;}else confidence=0;
    MotionOutput[pixel]=mv;DepthOutput[pixel]=depth;SelectionOutput[pixel]=confidence;
}
