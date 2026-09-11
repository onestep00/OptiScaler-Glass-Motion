cbuffer Frame : register(b0) { float time; float cameraX; float cameraY; float unused; };
struct Output { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float clip:SV_ClipDistance; };
Output main(float3 position:POSITION, float2 uv:TEXCOORD0, uint object:OBJECT_ID) {
    Output o;
    float3 p=position;
    p.x += sin(time + p.y*3.0 + float(object))*0.07 + float(object)*0.18-0.18;
    p.y += cos(time*1.3+p.x*2.0+float(object))*0.05;
    float w=1.0 + p.y*0.2 + float(object)*0.03;
    o.position=float4((p.x-cameraX)*0.7,(p.y-cameraY)*0.7,(0.4+float(object)*0.03)*w,w);
    o.uv=uv; o.tint=float3(0.2+float(object)*0.2,0.5,0.9); o.clip=1;
    return o;
}
