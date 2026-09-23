// Fixture: the audited Cyberpunk transparent-VS camera block.
// b1/space0 with 848 bytes (53 rows of 16) keeps row 51 inside the declared
// range. The pair capture stores row 51 XY in every record tag, so the pixel
// shader can add the frame-to-frame difference of those two words.
cbuffer Camera : register(b1, space0)
{
    float4 rows[53];
};

float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 p = id == 0 ? float2(-1, -1) : id == 1 ? float2(-1, 3) : float2(3, -1);
    // Keep the camera block live without moving the fixture's clip position:
    // the multiplier is zero for every vertex id the fixture submits.
    float zero = (id == 0xFFFFFFFFu) ? 1.0f : 0.0f;
    return float4(p + rows[51].xy * zero, 0.5, 1);
}
