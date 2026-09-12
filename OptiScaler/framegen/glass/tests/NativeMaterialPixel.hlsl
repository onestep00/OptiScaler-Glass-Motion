struct Input { float4 position : SV_Position; float4 current : TEXCOORD0; float4 previous : TEXCOORD1; };
float4 main(Input i) : SV_Target {
    // Visible hole and spatially varying transmission exercise original discard
    // and material blending independently of the native clip inputs.
    if (i.position.y > 8 && i.position.y < 10) discard;
    return float4(0.2, 0.4, 0.7, saturate(i.position.x / 32));
}
