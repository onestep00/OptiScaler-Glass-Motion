float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0,float3 tint:TEXCOORD1):SV_Target {
    float a=saturate(1.0-length((uv-0.5)*2.0));
    clip(a-0.13);
    clip(abs(uv.x-0.5)-0.05);
    return float4(tint*(0.5+uv.y*0.5),a);
}
