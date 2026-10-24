// Keep the authored cutout, discard the authored colour. Each source mip is
// converted independently so hair coverage does not change with distance.
Texture2D<float4> source : register(t0);
RWTexture2D<unorm float4> target : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);
    if (pixel.x < width && pixel.y < height) {
        target[pixel.xy] = float4(1, 1, 1, source.Load(int3(pixel.xy, 0)).a);
    }
}
