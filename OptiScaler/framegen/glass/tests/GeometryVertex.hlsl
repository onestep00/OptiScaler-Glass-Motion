cbuffer Frame : register(b0) { float time; float cameraX; float cameraY; float unused; };
struct Output { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float clip:SV_ClipDistance; };
Output main(float3 position:POSITION, float2 uv:TEXCOORD0, uint instance:SV_InstanceID) {
    Output o;
    // Deliberately time-dependent, non-rigid geometry, instance translation,
    // camera motion and perspective. History must include all of these.
    float3 p=position;
    p.x += sin(time + p.y*3.0)*0.07 + float(instance)*0.65-0.65;
    p.y += cos(time*1.3+p.x*2.0)*0.05;
    float w=1.0 + p.y*0.2 + float(instance)*0.03;
    o.position=float4((p.x-cameraX)*0.7,(p.y-cameraY)*0.7,0.4*w,w);
    o.uv=uv;o.tint=float3(0.2+float(instance)*0.2,0.5,0.9);
    // VS-only system output reproduces the game's unequal VS/PS signature size.
    o.clip=1;
    return o;
}
