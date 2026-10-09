// PlanetErosionGpu.inl
//
// Implementation of the PlanetErosion plugin module. It is compiled ONLY by
// the plugin (Plugins/PlanetErosion/Source/PlanetErosion/Private/
// PlanetErosionModule.cpp includes it); UBT does not compile .inl files of the
// game module, so keeping it here costs nothing and keeps it in git.
//
// The module loads at PostConfigInit: global shader types must be registered
// before the engine initialises its shader types, which is why this cannot
// live in the game module.
#include "PlanetErosionGpu.h"

#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "Engine/TextureRenderTarget2D.h"

class FPlanetErodeCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FPlanetErodeCS);
    SHADER_USE_PARAMETER_STRUCT(FPlanetErodeCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPlanetVertexIn>, Vertices)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, DeltaOut)
        SHADER_PARAMETER(FVector4f, ErosionP0)
        SHADER_PARAMETER(FVector4f, ErosionRounding)
        SHADER_PARAMETER(FVector4f, ErosionOnset)
        SHADER_PARAMETER(FVector4f, ErosionP3)
        SHADER_PARAMETER(FVector4f, ErosionP4)
        SHADER_PARAMETER(int32, ErosionOctaves)
        SHADER_PARAMETER(uint32, ErosionSeed)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};
IMPLEMENT_GLOBAL_SHADER(FPlanetErodeCS, "/PlanetErosion/PlanetErosion.usf", "ErodeCS", SF_Compute);

class FPlanetWriteCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FPlanetWriteCS);
    SHADER_USE_PARAMETER_STRUCT(FPlanetWriteCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPlanetVertexIn>, Vertices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPlanetTileIn>, Tiles)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, DeltaIn)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutPos)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutNormal)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutBiome)
        SHADER_PARAMETER(FVector4f, ClimateParams)
        SHADER_PARAMETER(uint32, TilesPerRow)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};
IMPLEMENT_GLOBAL_SHADER(FPlanetWriteCS, "/PlanetErosion/PlanetErosion.usf", "WriteCS", SF_Compute);

namespace PlanetErosionGpu
{

bool IsSupported()
{
    return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
}

void Dispatch(UTextureRenderTarget2D* PosAtlas, UTextureRenderTarget2D* NormalAtlas,
              UTextureRenderTarget2D* BiomeAtlas, int32 TilesPerRow,
              const FShaderParams& Params, FTileBatch&& Batch)
{
    check(IsInGameThread());
    if (!PosAtlas || !NormalAtlas || !BiomeAtlas || Batch.NumTiles <= 0) return;
    // The shaders exist only for SM5+ (ShouldCompilePermutation); asking for
    // them below that would assert. The game module falls back earlier.
    if (!IsSupported()) return;
    if (Batch.Vertices.Num() != Batch.NumTiles * HaloPoints * VertexBytes ||
        Batch.Tiles.Num()    != Batch.NumTiles * TileBytes)
    {
        UE_LOG(LogTemp, Error, TEXT("[PlanetErosion] batch size mismatch, %d tiles dropped"), Batch.NumTiles);
        return;
    }

    FTextureRenderTargetResource* PosRes    = PosAtlas->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* NormalRes = NormalAtlas->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* BiomeRes  = BiomeAtlas->GameThread_GetRenderTargetResource();
    if (!PosRes || !NormalRes || !BiomeRes) return;

    ENQUEUE_RENDER_COMMAND(PlanetErosionDispatch)(
        [PosRes, NormalRes, BiomeRes, TilesPerRow, Params, Batch = MoveTemp(Batch)](FRHICommandListImmediate& RHICmdList)
        {
            FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            TShaderMapRef<FPlanetErodeCS> ErodeCS(ShaderMap);
            TShaderMapRef<FPlanetWriteCS> WriteCS(ShaderMap);
            if (!ErodeCS.IsValid() || !WriteCS.IsValid()) return;

            FRHITexture* PosRHI    = PosRes->GetRenderTargetTexture();
            FRHITexture* NormalRHI = NormalRes->GetRenderTargetTexture();
            FRHITexture* BiomeRHI  = BiomeRes->GetRenderTargetTexture();
            if (!PosRHI || !NormalRHI || !BiomeRHI) return;

            FRDGBuilder GraphBuilder(RHICmdList);
            const uint32 NumTiles  = (uint32)Batch.NumTiles;
            const uint32 NumPoints = NumTiles * (uint32)HaloPoints;

            // NoCopy: the batch lives in this lambda until Execute() returns.
            FRDGBufferRef VertexBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Planet.Erosion.Vertices"),
                VertexBytes, NumPoints, Batch.Vertices.GetData(), Batch.Vertices.Num(), ERDGInitialDataFlags::NoCopy);
            FRDGBufferRef TileBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Planet.Erosion.Tiles"),
                TileBytes, NumTiles, Batch.Tiles.GetData(), Batch.Tiles.Num(), ERDGInitialDataFlags::NoCopy);
            FRDGBufferRef DeltaBuf = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumPoints), TEXT("Planet.Erosion.Delta"));

            FRDGTextureRef PosTex    = RegisterExternalTexture(GraphBuilder, PosRHI,    TEXT("PlanetPosAtlas"));
            FRDGTextureRef NormalTex = RegisterExternalTexture(GraphBuilder, NormalRHI, TEXT("PlanetNormalAtlas"));
            FRDGTextureRef BiomeTex  = RegisterExternalTexture(GraphBuilder, BiomeRHI,  TEXT("PlanetBiomeAtlas"));

            FRDGBufferSRVRef VertexSRV = GraphBuilder.CreateSRV(VertexBuf);

            // Pass 1: erosion delta of every halo-grid point.
            {
                FPlanetErodeCS::FParameters* P = GraphBuilder.AllocParameters<FPlanetErodeCS::FParameters>();
                P->Vertices        = VertexSRV;
                P->DeltaOut        = GraphBuilder.CreateUAV(DeltaBuf);
                P->ErosionP0       = Params.P0;
                P->ErosionRounding = Params.Rounding;
                P->ErosionOnset    = Params.Onset;
                P->ErosionP3       = Params.P3;
                P->ErosionP4       = Params.P4;
                P->ErosionOctaves  = Params.Octaves;
                P->ErosionSeed     = Params.Seed;
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PlanetErosion (%u tiles)", NumTiles),
                                             ErodeCS, P, FIntVector(9, 9, (int32)NumTiles));
            }

            // Pass 2: positions, normals and biomes into the atlases.
            {
                FPlanetWriteCS::FParameters* P = GraphBuilder.AllocParameters<FPlanetWriteCS::FParameters>();
                P->Vertices      = VertexSRV;
                P->Tiles         = GraphBuilder.CreateSRV(TileBuf);
                P->DeltaIn       = GraphBuilder.CreateSRV(DeltaBuf);
                P->OutPos        = GraphBuilder.CreateUAV(PosTex);
                P->OutNormal     = GraphBuilder.CreateUAV(NormalTex);
                P->OutBiome      = GraphBuilder.CreateUAV(BiomeTex);
                P->ClimateParams = Params.Climate;
                P->TilesPerRow   = (uint32)FMath::Max(1, TilesPerRow);
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PlanetTerrainTiles (%u tiles)", NumTiles),
                                             WriteCS, P, FIntVector(9, 9, (int32)NumTiles));
            }

            // The terrain material samples the atlases later this frame.
            GraphBuilder.SetTextureAccessFinal(PosTex,    ERHIAccess::SRVMask);
            GraphBuilder.SetTextureAccessFinal(NormalTex, ERHIAccess::SRVMask);
            GraphBuilder.SetTextureAccessFinal(BiomeTex,  ERHIAccess::SRVMask);
            GraphBuilder.Execute();
        });
}

} // namespace PlanetErosionGpu

class FPlanetErosionModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // The shaders stay in the game repository (GpuErosion/Shaders), so
        // they update with git like the rest of the planet code. Built at run
        // time (not baked in at compile time): safe for non-ASCII paths.
        const FString ShaderDir = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::GameSourceDir(), TEXT("FastNoiseTest"), TEXT("GpuErosion"), TEXT("Shaders")));
        if (!FPaths::DirectoryExists(ShaderDir))
        {
            // Without the mapping the engine cannot find the shaders and stops
            // anyway; say why in plain words first.
            UE_LOG(LogTemp, Fatal, TEXT("[PlanetErosion] shader folder not found: %s. ")
                   TEXT("The planet code is expected in Source/FastNoiseTest/GpuErosion."), *ShaderDir);
            return;
        }
        AddShaderSourceDirectoryMapping(TEXT("/PlanetErosion"), ShaderDir);
    }
};

IMPLEMENT_MODULE(FPlanetErosionModule, PlanetErosion)
