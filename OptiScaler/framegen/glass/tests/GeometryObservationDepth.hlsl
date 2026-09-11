struct Output
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};
Output main()
{
    Output value;
    value.color = float4(1, 0, 0, 0.5);
    value.depth = 0.5;
    return value;
}
