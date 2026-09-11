struct Output { float4 position : SV_Position; float4 current : TEXCOORD0; float4 previous : TEXCOORD1; };
Output main(uint4 words : RAW_WORDS, uint vertex : SV_VertexID) {
    Output o;
    float2 p = vertex == 0 ? float2(-0.5,-0.5) : vertex == 1 ? float2(0,0.5) : float2(0.5,-0.5);
    p += float2(words.xy) * 0.001;
    float w = vertex + 2;
    o.current = float4(p*w, 0.5*w, w);
    o.previous = float4((p+float2(0.125,-0.25))*(w+1),0.25*(w+1),w+1);
    o.position = o.current;
    o.position.x += 0.0625*w;
    return o;
}
