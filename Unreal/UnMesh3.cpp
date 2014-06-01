#include "Core.h"

#if UNREAL3

#include "UnrealClasses.h"
#include "UnMesh3.h"
#include "UnMeshTypes.h"
#include "UnMathTools.h"			// for FRotator to FCoords
#include "UnMaterial3.h"

#include "SkeletalMesh.h"
#include "StaticMesh.h"
#include "TypeConvert.h"


//#define DEBUG_SKELMESH		1
//#define DEBUG_STATICMESH		1


//?? move outside?
float half2float(word h)
{
	union
	{
		float		f;
		unsigned	df;
	} f;

	int sign = (h >> 15) & 0x00000001;
	int exp  = (h >> 10) & 0x0000001F;
	int mant =  h        & 0x000003FF;

	exp  = exp + (127 - 15);
	f.df = (sign << 31) | (exp << 23) | (mant << 13);
	return f.f;
}


static void UnpackNormals(const FPackedNormal SrcNormal[3], CMeshVertex &V)
{
	// tangents: convert to FVector (unpack) then cast to CVec3
	FVector Tangent = SrcNormal[0];
	FVector Normal  = SrcNormal[2];
	V.Tangent = CVT(Tangent);
	V.Normal  = CVT(Normal);
	if (SrcNormal[1].Data == 0)
	{
		// new UE3 version - this normal is not serialized and restored in vertex shader
		// LocalVertexFactory.usf, VertexFactoryGetTangentBasis() (static mesh)
		// GpuSkinVertexFactory.usf, SkinTangents() (skeletal mesh)
		cross(V.Normal, V.Tangent, V.Binormal);
		if (SrcNormal[2].GetW() == -1)
			V.Binormal.Negate();
	}
	else
	{
		// unpack Binormal
		FVector Binormal = SrcNormal[1];
		V.Binormal = CVT(Binormal);
	}
}



/*-----------------------------------------------------------------------------
	USkeletalMesh
-----------------------------------------------------------------------------*/

#define NUM_INFLUENCES_UE3			4
#define NUM_UV_SETS_UE3				4


#if NUM_INFLUENCES_UE3 != NUM_INFLUENCES
#error NUM_INFLUENCES_UE3 and NUM_INFLUENCES are not matching!
#endif

#if NUM_UV_SETS_UE3 != NUM_MESH_UV_SETS
#error NUM_UV_SETS_UE3 and NUM_MESH_UV_SETS are not matching!
#endif


// Implement constructor in cpp to avoid inlining (it's large enough).
// It's useful to declare TArray<> structures as forward declarations in header file.
USkeletalMesh3::USkeletalMesh3()
:	bHasVertexColors(false)
#if BATMAN
,	EnableTwistBoneFixers(true)
,	EnableClavicleFixer(true)
#endif
{}


USkeletalMesh3::~USkeletalMesh3()
{
	delete ConvertedMesh;
}


struct FSkelMeshSection3
{
	short				MaterialIndex;
	short				unk1;
	int					FirstIndex;
	int					NumTriangles;
	byte				unk2;

	friend FArchive& operator<<(FArchive &Ar, FSkelMeshSection3 &S)
	{
		guard(FSkelMeshSection3<<);
		if (Ar.ArVer < 215)
		{
			// UE2 fields
			short FirstIndex;
			short unk1, unk2, unk3, unk4, unk5, unk6, unk7;
			TArray<short> unk8;
			Ar << S.MaterialIndex << FirstIndex << unk1 << unk2 << unk3 << unk4 << unk5 << unk6 << S.NumTriangles;
			if (Ar.ArVer < 202) Ar << unk8;	// ArVer<202 -- from EndWar
			S.FirstIndex = FirstIndex;
			S.unk1 = 0;
			return Ar;
		}
		Ar << S.MaterialIndex << S.unk1 << S.FirstIndex;
#if BATMAN
		if (Ar.Game == GAME_Batman3) goto old_section; // Batman1 and 2 has version smaller than 806
#endif
		if (Ar.ArVer < 806)
		{
		old_section:
			// NumTriangles is unsigned short
			word NumTriangles;
			Ar << NumTriangles;
			S.NumTriangles = NumTriangles;
		}
		else
		{
			// NumTriangles is int
			Ar << S.NumTriangles;
		}
#if MCARTA
		if (Ar.Game == GAME_MagnaCarta && Ar.ArLicenseeVer >= 20)
		{
			int fC;
			Ar << fC;
		}
#endif // MCARTA
#if BLADENSOUL
		if (Ar.Game == GAME_BladeNSoul && Ar.ArVer >= 571) goto new_ver;
#endif
		if (Ar.ArVer >= 599)
		{
		new_ver:
			Ar << S.unk2;
		}
#if MASSEFF
		if (Ar.Game == GAME_MassEffect3 && Ar.ArLicenseeVer >= 135)
		{
			byte SomeFlag;
			Ar << SomeFlag;
			assert(SomeFlag == 0);
			// has extra data here in a case of SomeFlag != 0
		}
#endif // MASSEFF
#if BIOSHOCK3
		if (Ar.Game == GAME_Bioshock3)
		{
			//!! the same code as for MassEffect3, combine?
			byte SomeFlag;
			Ar << SomeFlag;
			assert(SomeFlag == 0);
			// has extra data here in a case of SomeFlag != 0
		}
#endif // BIOSHOCK3
#if REMEMBER_ME
		if (Ar.Game == GAME_RememberMe && Ar.ArLicenseeVer >= 17)
		{
			byte SomeFlag;
			TArray<byte> SomeArray;
			Ar << SomeFlag;
			if (SomeFlag) Ar << SomeArray;
		}
#endif // REMEMBER_ME
#if LOST_PLANET3
		if (Ar.Game == GAME_LostPlanet3 && Ar.ArLicenseeVer >= 75)
		{
			byte SomeFlag;
			Ar << SomeFlag;
			assert(SomeFlag == 0);	// array of 40-byte structures (serialized size = 36)
		}
#endif // LOST_PLANET3
#if XCOM_BUREAU
		if (Ar.Game == GAME_XcomB)
		{
			int SomeFlag;
			Ar << SomeFlag;
			assert(SomeFlag == 0);	// extra data
		}
#endif // XCOM_BUREAU
		return Ar;
		unguard;
	}
};

// This class is used now for UStaticMesh only
struct FIndexBuffer3
{
	TArray<word>		Indices;

	friend FArchive& operator<<(FArchive &Ar, FIndexBuffer3 &I)
	{
		guard(FIndexBuffer3<<);

		int unk;						// Revision?
		Ar << RAW_ARRAY(I.Indices);
		if (Ar.ArVer < 297) Ar << unk;	// at older version compatible with FRawIndexBuffer
		return Ar;

		unguard;
	}
};

// real name (from Android version): FRawStaticIndexBuffer
struct FSkelIndexBuffer3				// differs from FIndexBuffer3 since version 806 - has ability to store int indices
{
	TArray<word>		Indices16;
	TArray<unsigned>	Indices32;

	FORCEINLINE bool Is32Bit() const
	{
		return (Indices32.Num() != 0);
	}

	friend FArchive& operator<<(FArchive &Ar, FSkelIndexBuffer3 &I)
	{
		guard(FSkelIndexBuffer3<<);

		byte ItemSize = 2;

#if BATMAN
		if ((Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3) && Ar.ArLicenseeVer >= 45)
		{
			int unk34;
			Ar << unk34;
			goto old_index_buffer;
		}
#endif // BATMAN

		if (Ar.ArVer >= 806)
		{
			int		f0;
			Ar << f0 << ItemSize;
		}
#if PLA
		if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
		{
			FGuid unk;
			Ar << unk;
		}
#endif // PLA
	old_index_buffer:
		if (ItemSize == 2)
			Ar << RAW_ARRAY(I.Indices16);
		else if (ItemSize == 4)
			Ar << RAW_ARRAY(I.Indices32);
		else
			appError("Unknown ItemSize %d", ItemSize);

		int unk;
		if (Ar.ArVer < 297) Ar << unk;	// at older version compatible with FRawIndexBuffer

		return Ar;

		unguard;
	}
};

struct FRigidVertex3
{
	FVector				Pos;
	FPackedNormal		Normal[3];
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];
	byte				BoneIndex;
	int					Color;

	friend FArchive& operator<<(FArchive &Ar, FRigidVertex3 &V)
	{
		int NumUVSets = 1;

#if ENDWAR
		if (Ar.Game == GAME_EndWar)
		{
			// End War uses 4-component FVector everywhere, but here it is 3-component
			Ar << V.Pos.X << V.Pos.Y << V.Pos.Z;
			goto normals;
		}
#endif // ENDWAR
		Ar << V.Pos;
#if CRIMECRAFT
		if (Ar.Game == GAME_CrimeCraft)
		{
			if (Ar.ArLicenseeVer >= 1) Ar.Seek(Ar.Tell() + sizeof(float));
			if (Ar.ArLicenseeVer >= 2) NumUVSets = 4;
		}
#endif // CRIMECRAFT
		// note: version prior 477 have different normal/tangent format (same layout, but different
		// data meaning)
	normals:
		Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
#if R6VEGAS
		if (Ar.Game == GAME_R6Vegas2 && Ar.ArLicenseeVer >= 63)
		{
			FMeshUVHalf hUV;
			Ar << hUV;
			V.UV[0] = hUV;			// convert
			goto influences;
		}
#endif // R6VEGAS

		// UVs
		if (Ar.ArVer >= 709) NumUVSets = 4;
#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 13) NumUVSets = 3;
#endif
#if MKVSDC || STRANGLE || TRANSFORMERS
		if ((Ar.Game == GAME_MK && Ar.ArLicenseeVer >= 11) ||
			Ar.Game == GAME_Strangle ||	// Stranglehold check MidwayVer >= 17
			(Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55))
			NumUVSets = 2;
#endif
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines)
		{
			if (Ar.ArLicenseeVer >= 3 && Ar.ArLicenseeVer <= 52)	// Frontlines (the code in Homefront uses different version comparison!)
				NumUVSets = 2;
			else if (Ar.ArLicenseeVer > 52)
			{
				byte Num;
				Ar << Num;
				NumUVSets = Num;
			}
		}
#endif // FRONTLINES
		// UV
		for (int i = 0; i < NumUVSets; i++)
			Ar << V.UV[i];

		if (Ar.ArVer >= 710) Ar << V.Color;	// default 0xFFFFFFFF

#if MCARTA
		if (Ar.Game == GAME_MagnaCarta && Ar.ArLicenseeVer >= 5)
		{
			int f20;
			Ar << f20;
		}
#endif // MCARTA
	influences:
		Ar << V.BoneIndex;
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88)
		{
			int unk24;
			Ar << unk24;
		}
#endif // FRONTLINES
		return Ar;
	}
};


struct FSmoothVertex3
{
	FVector				Pos;
	FPackedNormal		Normal[3];
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];
	byte				BoneIndex[NUM_INFLUENCES_UE3];
	byte				BoneWeight[NUM_INFLUENCES_UE3];
	int					Color;

	friend FArchive& operator<<(FArchive &Ar, FSmoothVertex3 &V)
	{
		int i;
		int NumUVSets = 1;

#if ENDWAR
		if (Ar.Game == GAME_EndWar)
		{
			// End War uses 4-component FVector everywhere, but here it is 3-component
			Ar << V.Pos.X << V.Pos.Y << V.Pos.Z;
			goto normals;
		}
#endif // ENDWAR
		Ar << V.Pos;
#if CRIMECRAFT
		if (Ar.Game == GAME_CrimeCraft)
		{
			if (Ar.ArLicenseeVer >= 1) Ar.Seek(Ar.Tell() + sizeof(float));
			if (Ar.ArLicenseeVer >= 2) NumUVSets = 4;
		}
#endif // CRIMECRAFT
		// note: version prior 477 have different normal/tangent format (same layout, but different
		// data meaning)
	normals:
		Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
#if R6VEGAS
		if (Ar.Game == GAME_R6Vegas2 && Ar.ArLicenseeVer >= 63)
		{
			FMeshUVHalf hUV;
			Ar << hUV;
			V.UV[0] = hUV;			// convert
			goto influences;
		}
#endif // R6VEGAS

		// UVs
		if (Ar.ArVer >= 709) NumUVSets = 4;
#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 13) NumUVSets = 3;
#endif
#if MKVSDC || STRANGLE || TRANSFORMERS
		if ((Ar.Game == GAME_MK && Ar.ArLicenseeVer >= 11) ||
			Ar.Game == GAME_Strangle ||	// Stranglehold check MidwayVer >= 17
			(Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55))
			NumUVSets = 2;
#endif
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines)
		{
			if (Ar.ArLicenseeVer >= 3 && Ar.ArLicenseeVer <= 52)	// Frontlines (the code in Homefront uses different version comparison!)
				NumUVSets = 2;
			else if (Ar.ArLicenseeVer > 52)
			{
				byte Num;
				Ar << Num;
				NumUVSets = Num;
			}
		}
#endif // FRONTLINES
		// UV
		for (int i = 0; i < NumUVSets; i++)
			Ar << V.UV[i];

		if (Ar.ArVer >= 710) Ar << V.Color;	// default 0xFFFFFFFF

#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88)
		{
			int unk24;
			Ar << unk24;
		}
#endif // FRONTLINES

	influences:
		if (Ar.ArVer >= 333)
		{
			for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneIndex[i];
			for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneWeight[i];
		}
		else
		{
			for (i = 0; i < NUM_INFLUENCES_UE3; i++)
				Ar << V.BoneIndex[i] << V.BoneWeight[i];
		}
#if MCARTA
		if (Ar.Game == GAME_MagnaCarta && Ar.ArLicenseeVer >= 5)
		{
			int f28;
			Ar << f28;
		}
#endif // MCARTA
		return Ar;
	}
};

#if MKVSDC
struct FMesh3Unk4_MK
{
	word				data[4];

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk4_MK &S)
	{
		return Ar << S.data[0] << S.data[1] << S.data[2] << S.data[3];
	}
};
#endif // MKVSDC

// real name: FSkelMeshChunk
struct FSkinChunk3
{
	int					FirstVertex;
	TArray<FRigidVertex3>  RigidVerts;
	TArray<FSmoothVertex3> SmoothVerts;
	TArray<short>		Bones;
	int					NumRigidVerts;
	int					NumSmoothVerts;
	int					MaxInfluences;

	friend FArchive& operator<<(FArchive &Ar, FSkinChunk3 &V)
	{
		guard(FSkinChunk3<<);
		Ar << V.FirstVertex << V.RigidVerts << V.SmoothVerts;
#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 459)
		{
			TArray<FMesh3Unk4_MK> unk1C, unk28;
			Ar << unk1C << unk28;
		}
#endif // MKVSDC
		Ar << V.Bones;
		if (Ar.ArVer >= 333)
		{
			Ar << V.NumRigidVerts << V.NumSmoothVerts;
			// note: NumRigidVerts and NumSmoothVerts may be non-zero while corresponding
			// arrays are empty - that's when GPU skin only left
		}
		else
		{
			V.NumRigidVerts  = V.RigidVerts.Num();
			V.NumSmoothVerts = V.SmoothVerts.Num();
		}
#if ARMYOF2
		if (Ar.Game == GAME_ArmyOf2 && Ar.ArLicenseeVer >= 7)
		{
			TArray<FMeshUVFloat> extraUV;
			Ar << RAW_ARRAY(extraUV);
		}
#endif // ARMYOF2
		if (Ar.ArVer >= 362)
			Ar << V.MaxInfluences;
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55)
		{
			int NumTexCoords;
			Ar << NumTexCoords;
		}
#endif // TRANSFORMERS
#if DEBUG_SKELMESH
		appPrintf("Chunk: FirstVert=%d RigidVerts=%d (%d) SmoothVerts=%d (%d) MaxInfs=%d\n",
			V.FirstVertex, V.RigidVerts.Num(), V.NumRigidVerts, V.SmoothVerts.Num(), V.NumSmoothVerts, V.MaxInfluences);
#endif
		return Ar;
		unguard;
	}
};

struct FEdge3
{
	int					iVertex[2];
	int					iFace[2];

	friend FArchive& operator<<(FArchive &Ar, FEdge3 &V)
	{
#if BATMAN
		if (Ar.Game == GAME_Batman && Ar.ArLicenseeVer >= 5)
		{
			short iVertex[2], iFace[2];
			Ar << iVertex[0] << iVertex[1] << iFace[0] << iFace[1];
			V.iVertex[0] = iVertex[0];
			V.iVertex[1] = iVertex[1];
			V.iFace[0]   = iFace[0];
			V.iFace[1]   = iFace[1];
			return Ar;
		}
#endif // BATMAN
		return Ar << V.iVertex[0] << V.iVertex[1] << V.iFace[0] << V.iFace[1];
	}
};


// Structure holding normals and bone influeces
struct FGPUVert3Common
{
	FPackedNormal		Normal[3];
	byte				BoneIndex[NUM_INFLUENCES_UE3];
	byte				BoneWeight[NUM_INFLUENCES_UE3];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3Common &V)
	{
#if AVA
		if (Ar.Game == GAME_AVA) goto new_ver;
#endif
		if (Ar.ArVer < 494)
			Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
		else
		{
		new_ver:
			Ar << V.Normal[0] << V.Normal[2];
		}
#if CRIMECRAFT || FRONTLINES
		if ((Ar.Game == GAME_CrimeCraft && Ar.ArLicenseeVer >= 1) ||
			(Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88))
			Ar.Seek(Ar.Tell() + sizeof(float)); // pad or vertex color?
#endif
		int i;
		for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneIndex[i];
		for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneWeight[i];
		return Ar;
	}
};

static int GNumGPUUVSets = 1;

/*
 * Half = Float16
 * http://www.openexr.com/  source: ilmbase-*.tar.gz/Half/toFloat.cpp
 * http://en.wikipedia.org/wiki/Half_precision
 * Also look GL_ARB_half_float_pixel
 */
struct FGPUVert3Half : FGPUVert3Common
{
	FVector				Pos;
	FMeshUVHalf			UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3Half &V)
	{
		if (Ar.ArVer < 592)
			Ar << V.Pos << *((FGPUVert3Common*)&V);
		else
			Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FGPUVert3Float : FGPUVert3Common
{
	FVector				Pos;
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];

	FGPUVert3Float& operator=(const FSmoothVertex3 &S)
	{
		int i;
		Pos = S.Pos;
		Normal[0] = S.Normal[0];
		Normal[1] = S.Normal[1];
		Normal[2] = S.Normal[2];
		for (i = 0; i < NUM_MESH_UV_SETS; i++)
			UV[i] = S.UV[i];
		for (i = 0; i < NUM_INFLUENCES_UE3; i++)
		{
			BoneIndex[i]  = S.BoneIndex[i];
			BoneWeight[i] = S.BoneWeight[i];
		}
		return *this;
	}

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3Float &V)
	{
		if (Ar.ArVer < 592)
			Ar << V.Pos << *((FGPUVert3Common*)&V);
		else
			Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FGPUVert3PackedHalf : FGPUVert3Common
{
	FVectorIntervalFixed32GPU Pos;
	FMeshUVHalf			UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3PackedHalf &V)
	{
		Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FGPUVert3PackedFloat : FGPUVert3Common
{
	FVectorIntervalFixed32GPU Pos;
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3PackedFloat &V)
	{
		Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

// real name: FSkeletalMeshVertexBuffer
struct FGPUSkin3
{
	int							NumUVSets;
	int							bUseFullPrecisionUVs;		// 0 = half, 1 = float; copy of corresponding USkeletalMesh field
	// compressed position data
	int							bUsePackedPosition;			// 1 = packed FVector (32-bit), 0 = FVector (96-bit)
	FVector						MeshOrigin;
	FVector						MeshExtension;
	// vertex sets
	TArray<FGPUVert3Half>		VertsHalf;					// only one of these vertex sets are used
	TArray<FGPUVert3Float>		VertsFloat;
	TArray<FGPUVert3PackedHalf> VertsHalfPacked;
	TArray<FGPUVert3PackedFloat> VertsFloatPacked;

	inline int GetVertexCount() const
	{
		if (VertsHalf.Num()) return VertsHalf.Num();
		if (VertsFloat.Num()) return VertsFloat.Num();
		if (VertsHalfPacked.Num()) return VertsHalfPacked.Num();
		if (VertsFloatPacked.Num()) return VertsFloatPacked.Num();
		return 0;
	}

	friend FArchive& operator<<(FArchive &Ar, FGPUSkin3 &S)
	{
		guard(FGPUSkin3<<);

	#if DEBUG_SKELMESH
		appPrintf("Reading GPU skin\n");
	#endif
		if (Ar.IsLoading) S.bUsePackedPosition = false;
		bool AllowPackedPosition = false;
		S.NumUVSets = GNumGPUUVSets = 1;

	#if HUXLEY
		if (Ar.Game == GAME_Huxley) goto old_version;
	#endif
	#if AVA
		if (Ar.Game == GAME_AVA)
		{
			// different ArVer to check
			if (Ar.ArVer < 441) goto old_version;
			else				goto new_version;
		}
	#endif // AVA
	#if FRONTLINES
		if (Ar.Game == GAME_Frontlines)
		{
			if (Ar.ArLicenseeVer < 11)
				goto old_version;
			if (Ar.ArVer < 493 )
				S.bUseFullPrecisionUVs = true;
			else
				Ar << S.bUseFullPrecisionUVs;
			int VertexSize, NumVerts;
			Ar << S.NumUVSets << VertexSize << NumVerts;
			GNumGPUUVSets = S.NumUVSets;
			goto serialize_verts;
		}
	#endif // FRONTLINES

	#if ARMYOF2
		if (Ar.Game == GAME_ArmyOf2 && Ar.ArLicenseeVer >= 74)
		{
			int UseNewFormat;
			Ar << UseNewFormat;
			if (UseNewFormat)
			{
				appError("ArmyOfTwo: new vertex format!");
				return Ar;
			}
		}
	#endif // ARMYOF2
		if (Ar.ArVer < 493)
		{
		old_version:
			// old version - FSmoothVertex3 array
			TArray<FSmoothVertex3> Verts;
			Ar << RAW_ARRAY(Verts);
	#if DEBUG_SKELMESH
			appPrintf("... %d verts in old format\n", Verts.Num());
	#endif
			// convert verts
			CopyArray(S.VertsFloat, Verts);
			S.bUseFullPrecisionUVs = true;
			return Ar;
		}

		// new version
	new_version:
		// serialize type information
	#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 15) goto get_UV_count;
	#endif
	#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55) goto get_UV_count; // 1 or 2
	#endif
		if (Ar.ArVer >= 709)
		{
		get_UV_count:
			Ar << S.NumUVSets;
			GNumGPUUVSets = S.NumUVSets;
		}
		Ar << S.bUseFullPrecisionUVs;
		if (Ar.ArVer >= 592)
			Ar << S.bUsePackedPosition << S.MeshExtension << S.MeshOrigin;

		if (Ar.Platform == PLATFORM_XBOX360 || Ar.Platform == PLATFORM_PS3) AllowPackedPosition = true;

	#if MOH2010
		if (Ar.Game == GAME_MOH2010) AllowPackedPosition = true;
	#endif
	#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 573) GNumGPUUVSets = S.NumUVSets = 2;	// Injustice
	#endif
	#if CRIMECRAFT
		if (Ar.Game == GAME_CrimeCraft && Ar.ArLicenseeVer >= 2) S.NumUVSets = GNumGPUUVSets = 4;
	#endif
	#if LOST_PLANET3
		if (Ar.Game == GAME_LostPlanet3 && Ar.ArLicenseeVer >= 75)
		{
			int NoUV;
			Ar << NoUV;
			assert(!NoUV);	// when NoUV - special vertex format used: FCompNormal[2] + FVector (or something like this?)
		}
	#endif // LOST_PLANET3

		// UE3 PC version ignored bUsePackedPosition - forced !bUsePackedPosition in FGPUSkin3 serializer.
		// Note: in UDK (newer engine) there is no code to serialize GPU vertex with packed position.
		// Working bUsePackedPosition version was found in all XBox360 games. For PC there is only one game -
		// MOH2010, which uses bUsePackedPosition. PS3 also has bUsePackedPosition support (at least TRON)
	#if DEBUG_SKELMESH
		appPrintf("... data: packUV:%d packVert:%d numUV:%d PackPos:(%g %g %g)+(%g %g %g)\n",
			!S.bUseFullPrecisionUVs, S.bUsePackedPosition, S.NumUVSets,
			FVECTOR_ARG(S.MeshOrigin), FVECTOR_ARG(S.MeshExtension));
	#endif
		if (!AllowPackedPosition) S.bUsePackedPosition = false;		// not used in games (see comment above)

	#if PLA
		if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
		{
			FGuid unk;
			Ar << unk;
		}
	#endif // PLA

	serialize_verts:
		// serialize vertex array
		if (!S.bUseFullPrecisionUVs)
		{
			if (!S.bUsePackedPosition)
				Ar << RAW_ARRAY(S.VertsHalf);
			else
				Ar << RAW_ARRAY(S.VertsHalfPacked);
		}
		else
		{
			if (!S.bUsePackedPosition)
				Ar << RAW_ARRAY(S.VertsFloat);
			else
				Ar << RAW_ARRAY(S.VertsFloatPacked);
		}
	#if DEBUG_SKELMESH
		appPrintf("... verts: Half[%d] HalfPacked[%d] Float[%d] FloatPacked[%d]\n",
			S.VertsHalf.Num(), S.VertsHalfPacked.Num(), S.VertsFloat.Num(), S.VertsFloatPacked.Num());
	#endif

		return Ar;
		unguard;
	}
};

// real name: FVertexInfluence
struct FMesh3Unk1
{
	int					f0;
	int					f4;

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk1 &S)
	{
		return Ar << S.f0 << S.f4;
	}
};

SIMPLE_TYPE(FMesh3Unk1, int)

struct FMesh3Unk3
{
	int					f0;
	int					f4;
	TArray<word>		f8;

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk3 &S)
	{
		Ar << S.f0 << S.f4 << S.f8;
		return Ar;
	}
};

struct FMesh3Unk3A
{
	int					f0;
	int					f4;
	TArray<int>			f8;

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk3A &S)
	{
		Ar << S.f0 << S.f4 << S.f8;
		return Ar;
	}
};

struct FSkeletalMeshVertexInfluences
{
	TArray<FMesh3Unk1>	f0;
	TArray<FMesh3Unk3>	fC;				// Map or Set
	TArray<FMesh3Unk3A>	fCA;
	TArray<FSkelMeshSection3> Sections;
	TArray<FSkinChunk3>	Chunks;
	TArray<byte>		f80;
	byte				f8C;			// default = 0

	friend FArchive& operator<<(FArchive &Ar, FSkeletalMeshVertexInfluences &S)
	{
		guard(FSkeletalMeshVertexInfluences<<);
#if DEBUG_SKELMESH
		appPrintf("Extra vertex influence:\n");
#endif
		Ar << S.f0;
		if (Ar.ArVer >= 609)
		{
			if (Ar.ArVer >= 808)
			{
				Ar << S.fCA;
			}
			else
			{
				byte unk1;
				if (Ar.ArVer >= 806) Ar << unk1;
				Ar << S.fC;
			}
		}
		if (Ar.ArVer >= 700) Ar << S.Sections << S.Chunks;
		if (Ar.ArVer >= 708) Ar << S.f80;
		if (Ar.ArVer >= 715) Ar << S.f8C;
#if DEBUG_SKELMESH
		for (int i1 = 0; i1 < S.Sections.Num(); i1++)
		{
			FSkelMeshSection3 &Sec = S.Sections[i1];
			appPrintf("Sec[%d]: M=%d, FirstIdx=%d, NumTris=%d Unk=%d\n", i1, Sec.MaterialIndex, Sec.FirstIndex, Sec.NumTriangles, Sec.unk1);
		}

#endif // DEBUG_SKELMESH
		return Ar;
		unguard;
	}
};

#if R6VEGAS

struct FMesh3R6Unk1
{
	byte				f[6];

	friend FArchive& operator<<(FArchive &Ar, FMesh3R6Unk1 &S)
	{
		Ar << S.f[0] << S.f[1] << S.f[2] << S.f[3] << S.f[4];
		if (Ar.ArLicenseeVer >= 47) Ar << S.f[5];
		return Ar;
	}
};

#endif // R6VEGAS

#if TRANSFORMERS

struct FTRMeshUnkStream
{
	int					ItemSize;
	int					NumVerts;
	TArray<int>			Data;			// TArray<FPackedNormal>

	friend FArchive& operator<<(FArchive &Ar, FTRMeshUnkStream &S)
	{
		Ar << S.ItemSize << S.NumVerts;
		if (S.ItemSize && S.NumVerts)
			Ar << RAW_ARRAY(S.Data);
		return Ar;
	}
};

#endif // TRANSFORMERS

// Version references: 180..240 - Rainbow 6: Vegas 2
// Other: GOW PC
struct FStaticLODModel3
{
	TArray<FSkelMeshSection3> Sections;
	TArray<FSkinChunk3>	Chunks;
	FSkelIndexBuffer3	IndexBuffer;
	TArray<short>		UsedBones;		// bones, value = [0, NumBones-1]
	TArray<byte>		f24;			// count = NumBones, value = [0, NumBones-1]; note: BoneIndex is 'short', not 'byte' ...
	TArray<word>		f68;			// indices, value = [0, NumVertices-1]
	TArray<byte>		f74;			// count = NumTriangles
	int					f80;
	int					NumVertices;
	TArray<FEdge3>		Edges;			// links 2 vertices and 2 faces (triangles)
	FWordBulkData		BulkData;		// ElementCount = NumVertices
	FIntBulkData		BulkData2;		// used instead of BulkData since version 806, indices?
	FGPUSkin3			GPUSkin;
	TArray<FSkeletalMeshVertexInfluences> fC4;	// GoW2+ engine
	int					NumUVSets;
	TArray<int>			VertexColor;	// since version 710

	friend FArchive& operator<<(FArchive &Ar, FStaticLODModel3 &Lod)
	{
		guard(FStaticLODModel3<<);

#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 8)
		{
			// TLazyArray-like file pointer
			int EndArPos;
			Ar << EndArPos;
		}
#endif // FURY

#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 472 && Ar.Platform == PLATFORM_PS3)	//?? remove this code, doesn't work anyway
		{
			// this platform has no IndexBuffer
			Ar << Lod.Sections;
			goto part1;
		}
#endif // MKVSDC

		Ar << Lod.Sections << Lod.IndexBuffer;
#if DEBUG_SKELMESH
		for (int i1 = 0; i1 < Lod.Sections.Num(); i1++)
		{
			FSkelMeshSection3 &S = Lod.Sections[i1];
			appPrintf("Sec[%d]: M=%d, FirstIdx=%d, NumTris=%d Unk=%d\n", i1, S.MaterialIndex, S.FirstIndex, S.NumTriangles, S.unk1);
		}
		appPrintf("Indices: %d (16) / %d (32)\n", Lod.IndexBuffer.Indices16.Num(), Lod.IndexBuffer.Indices32.Num());
#endif // DEBUG_SKELMESH

		if (Ar.ArVer < 215)
		{
			TArray<FRigidVertex3>  RigidVerts;
			TArray<FSmoothVertex3> SmoothVerts;
			Ar << SmoothVerts << RigidVerts;
			appNotify("SkeletalMesh: untested code! (ArVer=%d)", Ar.ArVer);
		}

#if ENDWAR || BORDERLANDS
		if (Ar.Game == GAME_EndWar || (Ar.Game == GAME_Borderlands && Ar.ArLicenseeVer >= 57))	//!! Borderlands version unknown
		{
			// refined field set
			Ar << Lod.UsedBones << Lod.Chunks << Lod.f80 << Lod.NumVertices;
			goto part2;
		}
#endif // ENDWAR || BORDERLANDS

#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArVer >= 536)
		{
			// Transformers: Dark of the Moon
			// refined field set + byte bone indices
			assert(Ar.ArLicenseeVer >= 152);		// has mixed version comparisons - ArVer >= 536 and ArLicenseeVer >= 152
			TArray<byte> UsedBones2;
			Ar << UsedBones2;
			CopyArray(Lod.UsedBones, UsedBones2);	// byte -> int
			Ar << Lod.Chunks << Lod.NumVertices;
			goto part2;
		}
#endif // TRANSFORMERS

	part1:
		if (Ar.ArVer < 686) Ar << Lod.f68;
		Ar << Lod.UsedBones;
		if (Ar.ArVer < 686) Ar << Lod.f74;
		if (Ar.ArVer >= 215)
		{
		chunks:
			Ar << Lod.Chunks << Lod.f80 << Lod.NumVertices;
		}
#if DEBUG_SKELMESH
		appPrintf("%d chunks, %d bones, %d verts\n", Lod.Chunks.Num(), Lod.UsedBones.Num(), Lod.NumVertices);
#endif

#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 11)
		{
			int unk84;
			Ar << unk84;	// default is 1
		}
#endif

		if (Ar.ArVer < 686) Ar << Lod.Edges;

		if (Ar.ArVer < 202)
		{
#if 0
			// old version
			TLazyArray<FVertInfluence> Influences;
			TLazyArray<FMeshWedge>     Wedges;
			TLazyArray<FMeshFace>      Faces;
			TLazyArray<FVector>        Points;
			Ar << Influences << Wedges << Faces << Points;
#else
			appError("Old UE3 FStaticLodModel");
#endif
		}

#if STRANGLE
		if (Ar.Game == GAME_Strangle)
		{
			// also check MidwayTag == "WOO " and MidwayVer >= 346
			// f24 has been moved to the end
			Lod.BulkData.Serialize(Ar);
			Ar << Lod.GPUSkin;
			Ar << Lod.f24;
			return Ar;
		}
#endif // STRANGLE

#if DMC
		if (Ar.Game == GAME_DmC && Ar.ArLicenseeVer >= 3) goto word_f24;
#endif

	part2:
		if (Ar.ArVer >= 207)
		{
			Ar << Lod.f24;
		}
		else
		{
		word_f24:
			TArray<short> f24_a;
			Ar << f24_a;
		}
#if APB
		if (Ar.Game == GAME_APB)
		{
			// skip APB bulk; for details check UTexture3::Serialize()
			Ar.Seek(Ar.Tell() + 8);
			goto after_bulk;
		}
#endif // APB
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 181) // Transformers: Fall of Cybertron, no version in code
			goto after_bulk;
#endif
		/*!! PS3 MK:
			- no Bulk here
			- int NumSections (equals to Sections.Num())
			- int 2 or 4
			- 4 x int unknown
			- int SomeSize
			- byte[SomeSize]
			- word SomeSize (same as above)
			- ...
		*/
		if (Ar.ArVer >= 221)
		{
			if (Ar.ArVer < 806)
				Lod.BulkData.Serialize(Ar);		// Bulk of word
			else
				Lod.BulkData2.Serialize(Ar);	// Bulk of int
		}
	after_bulk:
#if R6VEGAS
		if (Ar.Game == GAME_R6Vegas2 && Ar.ArLicenseeVer >= 46)
		{
			TArray<FMesh3R6Unk1> unkA0;
			Ar << unkA0;
		}
#endif // R6VEGAS
#if ARMYOF2
		if (Ar.Game == GAME_ArmyOf2 && Ar.ArLicenseeVer >= 7)
		{
			int unk84;
			TArray<FMeshUVFloat> extraUV;
			Ar << unk84 << RAW_ARRAY(extraUV);
		}
#endif // ARMYOF2
#if BIOSHOCK3
		if (Ar.Game == GAME_Bioshock3)
		{
			int unkE4;
			Ar << unkE4;
		}
#endif // BIOSHOCK3
#if REMEMBER_ME
		if (Ar.Game == GAME_RememberMe && Ar.ArLicenseeVer >= 19)
		{
			int unkD4;
			Ar << unkD4;
		}
#endif // REMEMBER_ME
		if (Ar.ArVer >= 709)
			Ar << Lod.NumUVSets;
		else
			Lod.NumUVSets = 1;
#if DEBUG_SKELMESH
		appPrintf("NumUVSets=%d\n", Lod.NumUVSets);
#endif

#if MOH2010
		int RealArVer = Ar.ArVer;
		if (Ar.Game == GAME_MOH2010)
		{
			Ar.ArVer = 592;			// partially upgraded engine, override version (for easier coding)
			if (Ar.ArLicenseeVer >= 42)
				Ar << Lod.fC4;		// original code: this field is serialized after GPU Skin
		}
#endif // MOH2010
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 34)
		{
			int gpuSkinUnk;
			Ar << gpuSkinUnk;
		}
#endif // FURY
		if (Ar.ArVer >= 333)
			Ar << Lod.GPUSkin;
#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 459)
		{
			TArray<FMesh3Unk4_MK> unkA8;
			Ar << unkA8;
			return Ar;		// no extra fields
		}
#endif // MKVSDC
#if MOH2010
		if (Ar.Game == GAME_MOH2010)
		{
			Ar.ArVer = RealArVer;	// restore version
			if (Ar.ArLicenseeVer >= 42) return Ar;
		}
#endif
#if BLOODONSAND
		if (Ar.Game == GAME_50Cent) return Ar;	// new ArVer, but old engine
#endif
#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 15) return Ar;	// new ArVer, but old engine
#endif // MEDGE
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 73)
		{
			FTRMeshUnkStream unkStream;
			Ar << unkStream;
			return Ar;
		}
#endif // TRANSFORMERS
		if (Ar.ArVer >= 710)
		{
			USkeletalMesh3 *LoadingMesh = (USkeletalMesh3*)UObject::GLoadingObj;
			assert(LoadingMesh);
			if (LoadingMesh->bHasVertexColors)
			{
				Ar << RAW_ARRAY(Lod.VertexColor);
				appPrintf("WARNING: SkeletalMesh %s uses vertex colors\n", LoadingMesh->Name);
			}
		}
		if (Ar.ArVer >= 534)		// post-UT3 code
			Ar << Lod.fC4;
		if (Ar.ArVer >= 841)		// unknown extra index buffer
		{
			FSkelIndexBuffer3 unk;
			Ar << unk;
		}
//		assert(Lod.IndexBuffer.Indices.Num() == Lod.f68.Num()); -- mostly equals (failed in CH_TwinSouls_Cine.upk)
//		assert(Lod.BulkData.ElementCount == Lod.NumVertices); -- mostly equals (failed on some GoW packages)
		return Ar;

		unguard;
	}
};

#if A51 || MKVSDC || STRANGLE

struct FMaterialBone
{
	int					Bone;
	FName				Param;

	friend FArchive& operator<<(FArchive &Ar, FMaterialBone &V)
	{
		if (Ar.ArVer >= 573) // Injustice, version unknown
		{
			int f1[6], f2[6];
			Ar << f1[0] << f1[1] << f1[2] << f1[3] << f1[4] << f1[5];
			Ar << V.Param;
			Ar << f2[0] << f2[1] << f2[2] << f2[3] << f2[4] << f2[5];
			return Ar;
		}
		return Ar << V.Bone << V.Param;
	}
};

struct FSubSkeleton_MK
{
	FName				Name;
	int					BoneSet[8];			// FBoneSet

	friend FArchive& operator<<(FArchive &Ar, FSubSkeleton_MK &S)
	{
		Ar << S.Name;
		for (int i = 0; i < ARRAY_COUNT(S.BoneSet); i++) Ar << S.BoneSet[i];
		return Ar;
	}
};

struct FBoneMirrorInfo_MK
{
	int					SourceIndex;
	byte				BoneFlipAxis;		// EAxis

	friend FArchive& operator<<(FArchive &Ar, FBoneMirrorInfo_MK &M)
	{
		return Ar << M.SourceIndex << M.BoneFlipAxis;
	}
};

struct FReferenceSkeleton_MK
{
	TArray<VJointPos>	RefPose;
	TArray<short>		Parentage;
	TArray<FName>		BoneNames;
	TArray<FSubSkeleton_MK> SubSkeletons;
	TArray<FName>		UpperBoneNames;
	TArray<FBoneMirrorInfo_MK> SkelMirrorTable;
	byte				SkelMirrorAxis;		// EAxis
	byte				SkelMirrorFlipAxis;	// EAxis

	friend FArchive& operator<<(FArchive &Ar, FReferenceSkeleton_MK &S)
	{
		Ar << S.RefPose << S.Parentage << S.BoneNames;
		Ar << S.SubSkeletons;				// MidwayVer >= 56
		Ar << S.UpperBoneNames;				// MidwayVer >= 57
		Ar << S.SkelMirrorTable << S.SkelMirrorAxis << S.SkelMirrorFlipAxis;
		return Ar;
	}
};

#endif // MIDWAY ...

#if FURY
struct FSkeletalMeshLODInfoExtra
{
	int					IsForGemini;	// bool
	int					IsForTaurus;	// bool

	friend FArchive& operator<<(FArchive &Ar, FSkeletalMeshLODInfoExtra &V)
	{
		return Ar << V.IsForGemini << V.IsForTaurus;
	}
};
#endif // FURY

#if BATMAN
struct FBoneBounds
{
	int					BoneIndex;
	// FSimpleBox
	FVector				Min;
	FVector				Max;

	friend FArchive& operator<<(FArchive &Ar, FBoneBounds &B)
	{
		return Ar << B.BoneIndex << B.Min << B.Max;
	}
};
#endif // BATMAN

#if LEGENDARY

struct FSPAITag2
{
	UObject				*f0;
	int					f4;

	friend FArchive& operator<<(FArchive &Ar, FSPAITag2 &S)
	{
		Ar << S.f0;
		if (Ar.ArLicenseeVer < 10)
		{
			byte f4;
			Ar << f4;
			S.f4 = f4;
			return Ar;
		}
		int f4[9];		// serialize each bit of S.f4 as separate dword
		Ar << f4[0] << f4[1] << f4[2] << f4[3] << f4[4] << f4[5];
		if (Ar.ArLicenseeVer >= 23) Ar << f4[6];
		if (Ar.ArLicenseeVer >= 31) Ar << f4[7];
		if (Ar.ArLicenseeVer >= 34) Ar << f4[8];
		return Ar;
	}
};

#endif // LEGENDARY

void USkeletalMesh3::Serialize(FArchive &Ar)
{
	guard(USkeletalMesh3::Serialize);

#if FRONTLINES
	if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88)
	{
		int unk320;					// default 1
		Ar << unk320;
	}
#endif // FRONTLINES

	UObject::Serialize(Ar);			// no UPrimitive ...

#if MEDGE
	if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 15)
	{
		int unk264;
		Ar << unk264;
	}
#endif // MEDGE
#if FURY
	if (Ar.Game == GAME_Fury)
	{
		int b1, b2, b3, b4;		// bools, serialized as ints; LoadForGemini, LoadForTaurus, IsSceneryMesh, IsPlayerMesh
		TArray<FSkeletalMeshLODInfoExtra> LODInfoExtra;
		if (Ar.ArLicenseeVer >= 14) Ar << b1 << b2;
		if (Ar.ArLicenseeVer >= 8)
		{
			Ar << b3;
			Ar << LODInfoExtra;
		}
		if (Ar.ArLicenseeVer >= 15) Ar << b4;
	}
#endif // FURY
#if DISHONORED
	if (Ar.Game == GAME_Dishonored && Ar.ArVer >= 759)
	{
		// FUserBounds m_UserBounds
		FName   m_BoneName;
		FVector m_Offset;
		float   m_fRadius;
		Ar << m_BoneName;
		if (Ar.ArVer >= 761) Ar << m_Offset;
		Ar << m_fRadius;
	}
#endif // DISHONORED
	Ar << Bounds;
	if (Ar.ArVer < 180)
	{
		UObject *unk;
		Ar << unk;
	}
#if BATMAN
	if ((Ar.Game == GAME_Batman || Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3) && Ar.ArLicenseeVer >= 15)
	{
		float ConservativeBounds;
		TArray<FBoneBounds> PerBoneBounds;
		Ar << ConservativeBounds << PerBoneBounds;
	}
#endif // BATMAN
	Ar << Materials;
#if DEBUG_SKELMESH
	for (int i1 = 0; i1 < Materials.Num(); i1++)
		appPrintf("Material[%d] = %s\n", i1, Materials[i1] ? Materials[i1]->Name : "None");
#endif
#if BLOODONSAND
	if (Ar.Game == GAME_50Cent && Ar.ArLicenseeVer >= 65)
	{
		TArray<UObject*> OnFireMaterials;		// name is not checked
		Ar << OnFireMaterials;
	}
#endif // BLOODONSAND
#if DARKVOID
	if (Ar.Game == GAME_DarkVoid && Ar.ArLicenseeVer >= 61)
	{
		TArray<UObject*> AlternateMaterials;
		Ar << AlternateMaterials;
	}
#endif // DARKVOID
#if ALPHA_PR
	if (Ar.Game == GAME_AlphaProtocol && Ar.ArLicenseeVer >= 26)
	{
		TArray<int> SectionDepthBias;
		Ar << SectionDepthBias;
	}
#endif // ALPHA_PR
#if MKVSDC
	if (Ar.Game == GAME_MK && Ar.ArVer >= 472)	// MK, real version is unknown
	{
		FReferenceSkeleton_MK Skel;
		Ar << Skel << SkeletalDepth;
		MeshOrigin.Set(0, 0, 0);				// not serialized
		RotOrigin.Set(0, 0, 0);
		// convert skeleton
		int NumBones = Skel.RefPose.Num();
		assert(NumBones == Skel.Parentage.Num());
		assert(NumBones == Skel.BoneNames.Num());
		RefSkeleton.Add(NumBones);
		for (int i = 0; i < NumBones; i++)
		{
			FMeshBone &B = RefSkeleton[i];
			B.Name        = Skel.BoneNames[i];
			B.BonePos     = Skel.RefPose[i];
			B.ParentIndex = Skel.Parentage[i];
//			appPrintf("BONE: [%d] %s -> %d\n", i, *B.Name, B.ParentIndex);
		}
		goto after_skeleton;
	}
#endif // MKVSDC
#if ALIENS_CM
	if (Ar.Game == GAME_AliensCM && Ar.ArVer >= 21)	// && Ar.ExtraVer >= 1
	{
		TArray<UObject*> unk;
		Ar << unk;
	}
#endif // ALIENS_CM
	Ar << MeshOrigin << RotOrigin;
#if DISHONORED
	if (Ar.Game == GAME_Dishonored && Ar.ArVer >= 775)
	{
		TArray<byte> EdgeSkeleton;
		Ar << EdgeSkeleton;
	}
#endif // DISHONORED
	Ar << RefSkeleton << SkeletalDepth;
after_skeleton:
#if DEBUG_SKELMESH
	appPrintf("RefSkeleton: %d bones, %d depth\n", RefSkeleton.Num(), SkeletalDepth);
	for (int i1 = 0; i1 < RefSkeleton.Num(); i1++)
		appPrintf("  [%d] n=%s p=%d\n", i1, *RefSkeleton[i1].Name, RefSkeleton[i1].ParentIndex);
#endif // DEBUG_SKELMESH
#if A51 || MKVSDC || STRANGLE
	//?? check GAME_Wheelman
	if (Ar.Engine() == GAME_MIDWAY3 && Ar.ArLicenseeVer >= 0xF)
	{
		TArray<FMaterialBone> MaterialBones;
		Ar << MaterialBones;
	}
#endif // A51 || MKVSDC || STRANGLE
#if CRIMECRAFT
	if (Ar.Game == GAME_CrimeCraft && Ar.ArLicenseeVer >= 5)
	{
		byte unk8C;
		Ar << unk8C;
	}
#endif
#if LEGENDARY
	if (Ar.Game == GAME_Legendary && Ar.ArLicenseeVer >= 9)
	{
		TArray<FSPAITag2> OATStags2;
		Ar << OATStags2;
	}
#endif
#if SPECIALFORCE2
	if (Ar.Game == GAME_SpecialForce2 && Ar.ArLicenseeVer >= 14)
	{
		byte  unk108;
		FName unk10C;
		Ar << unk108 << unk10C;
	}
#endif
	Ar << LODModels;
#if 0
	//!! also: NameIndexMap (ArVer >= 296), PerPolyKDOPs (ArVer >= 435)
#else
	DROP_REMAINING_DATA(Ar);
#endif

	guard(ConvertMesh);

	CSkeletalMesh *Mesh = new CSkeletalMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;		//?? UE3 meshes has radius 2 times larger than mesh
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// MeshScale, MeshOrigin, RotOrigin
	VectorScale(CVT(MeshOrigin), -1, Mesh->MeshOrigin);
	Mesh->RotOrigin = RotOrigin;
	Mesh->MeshScale.Set(1, 1, 1);							// missing in UE3

	// convert LODs
	Mesh->Lods.Empty(LODModels.Num());
	assert(LODModels.Num() == LODInfo.Num());
	for (int lod = 0; lod < LODModels.Num(); lod++)
	{
		guard(ConvertLod);

		const FStaticLODModel3 &SrcLod = LODModels[lod];
		if (!SrcLod.Chunks.Num()) continue;

		int NumTexCoords = max(SrcLod.NumUVSets, SrcLod.GPUSkin.NumUVSets);		// number of texture coordinates is serialized differently for some games
		if (NumTexCoords > NUM_MESH_UV_SETS)
			appError("SkeletalMesh has %d UV sets", NumTexCoords);

		CSkelMeshLod *Lod = new (Mesh->Lods) CSkelMeshLod;
		Lod->NumTexCoords = NumTexCoords;
		Lod->HasNormals   = true;
		Lod->HasTangents  = true;

		guard(ProcessVerts);

		// get vertex count and determine vertex source
		int VertexCount = SrcLod.GPUSkin.GetVertexCount();
		bool UseGpuSkinVerts = (VertexCount > 0);
		if (!VertexCount)
		{
			const FSkinChunk3 &C = SrcLod.Chunks[SrcLod.Chunks.Num() - 1];		// last chunk
			VertexCount = C.FirstVertex + C.NumRigidVerts + C.NumSmoothVerts;
		}
		// allocate the vertices
		Lod->AllocateVerts(VertexCount);

		int chunkIndex = 0;
		const FSkinChunk3 *C = NULL;
		int lastChunkVertex = -1;
		const FGPUSkin3 &S = SrcLod.GPUSkin;
		CSkelMeshVertex *D = Lod->Verts;

		for (int Vert = 0; Vert < VertexCount; Vert++, D++)
		{
			if (Vert >= lastChunkVertex)
			{
				// proceed to next chunk
				C = &SrcLod.Chunks[chunkIndex++];
				lastChunkVertex = C->FirstVertex + C->NumRigidVerts + C->NumSmoothVerts;
			}

			if (UseGpuSkinVerts)
			{
				// NOTE: Gears3 has some issues:
				// - chunk may have FirstVertex set to incorrect value (for recent UE3 versions), which overlaps with the
				//   previous chunk (FirstVertex=0 for a few chunks)
				// - index count may be greater than sum of all face counts * 3 from all mesh sections -- this is verified in PSK exporter

				// get vertex from GPU skin
				const FGPUVert3Common *V;		// has normal and influences, but no UV[] and position

				if (!S.bUseFullPrecisionUVs)
				{
					// position
					const FMeshUVHalf *SUV;
					if (!S.bUsePackedPosition)
					{
						const FGPUVert3Half &V0 = S.VertsHalf[Vert];
						D->Position = CVT(V0.Pos);
						V   = &V0;
						SUV = V0.UV;
					}
					else
					{
						const FGPUVert3PackedHalf &V0 = S.VertsHalfPacked[Vert];
						FVector VPos;
						VPos = V0.Pos.ToVector(S.MeshOrigin, S.MeshExtension);
						D->Position = CVT(VPos);
						V   = &V0;
						SUV = V0.UV;
					}
					// UV
					for (int i = 0; i < NumTexCoords; i++)
					{
						FMeshUVFloat fUV = SUV[i];				// convert
						D->UV[i] = CVT(fUV);
					}
				}
				else
				{
					// position
					const FMeshUVFloat *SUV;
					if (!S.bUsePackedPosition)
					{
						const FGPUVert3Float &V0 = S.VertsFloat[Vert];
						V = &V0;
						D->Position = CVT(V0.Pos);
						SUV = V0.UV;
					}
					else
					{
						const FGPUVert3PackedFloat &V0 = S.VertsFloatPacked[Vert];
						V = &V0;
						FVector VPos;
						VPos = V0.Pos.ToVector(S.MeshOrigin, S.MeshExtension);
						D->Position = CVT(VPos);
						SUV = V0.UV;
					}
					// UV
					for (int i = 0; i < NumTexCoords; i++)
						D->UV[i] = CVT(SUV[i]);
				}
				// convert Normal[3]
				UnpackNormals(V->Normal, *D);
				// convert influences
//				int TotalWeight = 0;
				int i2 = 0;
				for (int i = 0; i < NUM_INFLUENCES_UE3; i++)
				{
					int BoneIndex  = V->BoneIndex[i];
					int BoneWeight = V->BoneWeight[i];
					if (BoneWeight == 0) continue;				// skip this influence (but do not stop the loop!)
					D->Weight[i2] = BoneWeight / 255.0f;
					D->Bone[i2]   = C->Bones[BoneIndex];
					i2++;
//					TotalWeight += BoneWeight;
				}
//				assert(TotalWeight = 255);
				if (i2 < NUM_INFLUENCES_UE3) D->Bone[i2] = INDEX_NONE; // mark end of list
			}
			else
			{
				// old UE3 version without a GPU skin
				// get vertex from chunk
				const FMeshUVFloat *SUV;
				if (Vert < C->FirstVertex + C->NumRigidVerts)
				{
					// rigid vertex
					const FRigidVertex3 &V0 = C->RigidVerts[Vert - C->FirstVertex];
					// position and normal
					D->Position = CVT(V0.Pos);
					UnpackNormals(V0.Normal, *D);
					// single influence
					D->Weight[0] = 1.0f;
					D->Bone[0]   = C->Bones[V0.BoneIndex];
					SUV = V0.UV;
				}
				else
				{
					// smooth vertex
					const FSmoothVertex3 &V0 = C->SmoothVerts[Vert - C->FirstVertex - C->NumRigidVerts];
					// position and normal
					D->Position = CVT(V0.Pos);
					UnpackNormals(V0.Normal, *D);
					// influences
//					int TotalWeight = 0;
					int i2 = 0;
					for (int i = 0; i < NUM_INFLUENCES_UE3; i++)
					{
						int BoneIndex  = V0.BoneIndex[i];
						int BoneWeight = V0.BoneWeight[i];
						if (BoneWeight == 0) continue;
						D->Weight[i2] = BoneWeight / 255.0f;
						D->Bone[i2]   = C->Bones[BoneIndex];
						i2++;
//						TotalWeight += BoneWeight;
					}
//					assert(TotalWeight = 255);
					if (i2 < NUM_INFLUENCES_UE3) D->Bone[i2] = INDEX_NONE; // mark end of list
					SUV = V0.UV;
				}
				// UV
				for (int i = 0; i < NumTexCoords; i++)
					D->UV[i] = CVT(SUV[i]);
			}
		}

		unguard;	// ProcessVerts

		// indices
		Lod->Indices.Initialize(&SrcLod.IndexBuffer.Indices16, &SrcLod.IndexBuffer.Indices32);

		// sections
		guard(ProcessSections);
		Lod->Sections.Empty(SrcLod.Sections.Num());
		assert(LODModels.Num() == LODInfo.Num());
		const FSkeletalMeshLODInfo &Info = LODInfo[lod];

		for (int Sec = 0; Sec < SrcLod.Sections.Num(); Sec++)
		{
			const FSkelMeshSection3 &S = SrcLod.Sections[Sec];
			CMeshSection *Dst = new (Lod->Sections) CMeshSection;

			int MaterialIndex = S.MaterialIndex;
			if (MaterialIndex >= 0 && MaterialIndex < Info.LODMaterialMap.Num())
				MaterialIndex = Info.LODMaterialMap[MaterialIndex];
			Dst->Material   = (MaterialIndex < Materials.Num()) ? Materials[MaterialIndex] : NULL;
			Dst->FirstIndex = S.FirstIndex;
			Dst->NumFaces   = S.NumTriangles;
		}

		unguard;	// ProcessSections

		unguardf(("lod=%d", lod)); // ConvertLod
	}

	// copy skeleton
	guard(ProcessSkeleton);
	Mesh->RefSkeleton.Empty(RefSkeleton.Num());
	for (int i = 0; i < RefSkeleton.Num(); i++)
	{
		const FMeshBone &B = RefSkeleton[i];
		CSkelMeshBone *Dst = new (Mesh->RefSkeleton) CSkelMeshBone;
		Dst->Name        = B.Name;
		Dst->ParentIndex = B.ParentIndex;
		Dst->Position    = CVT(B.BonePos.Position);
		Dst->Orientation = CVT(B.BonePos.Orientation);
		// fix skeleton; all bones but 0
		if (i >= 1)
			Dst->Orientation.w *= -1;
	}
	unguard; // ProcessSkeleton

	Mesh->FinalizeMesh();

	unguard; // ConvertMesh

#if BATMAN
	if (Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3)
		FixBatman2Skeleton();
#endif

	unguard;
}


void USkeletalMesh3::PostLoad()
{
	guard(USkeletalMesh3::PostLoad);

	assert(ConvertedMesh);

	int NumSockets = Sockets.Num();
	if (NumSockets)
	{
		ConvertedMesh->Sockets.Empty(NumSockets);
		for (int i = 0; i < NumSockets; i++)
		{
			USkeletalMeshSocket *S = Sockets[i];
			if (!S) continue;
			CSkelMeshSocket *DS = new (ConvertedMesh->Sockets) CSkelMeshSocket;
			DS->Name = S->SocketName;
			DS->Bone = S->BoneName;
			CCoords &C = DS->Transform;
			C.origin = CVT(S->RelativeLocation);
			SetAxis(S->RelativeRotation, C.axis);
		}
	}

	unguard;
}


/*-----------------------------------------------------------------------------
	UStaticMesh
-----------------------------------------------------------------------------*/

// Implement constructor in cpp to avoid inlining (it's large enough).
// It's useful to declare TArray<> structures as forward declarations in header file.
UStaticMesh3::UStaticMesh3()
{}

UStaticMesh3::~UStaticMesh3()
{
	delete ConvertedMesh;
}

#if TRANSFORMERS

struct FTRStaticMeshSectionUnk
{
	int					f0;
	int					f4;

	friend FArchive& operator<<(FArchive &Ar, FTRStaticMeshSectionUnk &S)
	{
		return Ar << S.f0 << S.f4;
	}
};

SIMPLE_TYPE(FTRStaticMeshSectionUnk, int)

#endif // TRANSFORMERS

#if MOH2010

struct FMOHStaticMeshSectionUnk
{
	TArray<int>			f4;
	TArray<int>			f10;
	TArray<short>		f1C;
	TArray<short>		f28;
	TArray<short>		f34;
	TArray<short>		f40;
	TArray<short>		f4C;
	TArray<short>		f58;

	friend FArchive& operator<<(FArchive &Ar, FMOHStaticMeshSectionUnk &S)
	{
		return Ar << S.f4 << S.f10 << S.f1C << S.f28 << S.f34 << S.f40 << S.f4C << S.f58;
	}
};

#endif // MOH2010


struct FStaticMeshSection3
{
	UMaterialInterface	*Mat;
	int					f10;		//?? bUseSimple...Collision
	int					f14;		//?? ...
	int					bEnableShadowCasting;
	int					FirstIndex;
	int					NumFaces;
	int					f24;		//?? first used vertex
	int					f28;		//?? last used vertex
	int					Index;		//?? index of section
	TArray<FMesh3Unk1>	f30;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshSection3 &S)
	{
		guard(FStaticMeshSection3<<);
#if TUROK
		if (Ar.Game == GAME_Turok && Ar.ArLicenseeVer >= 57)
		{
			// no S.Mat
			return Ar << S.f10 << S.f14 << S.FirstIndex << S.NumFaces << S.f24 << S.f28;
		}
#endif // TUROK
		Ar << S.Mat << S.f10 << S.f14;
#if MKVSDC
		if (Ar.Game == GAME_MK)
		{
			TArray<FGuid> unk28;				// 4x32-bit
			// no bEnableShadowCasting and Index fields
			Ar << S.FirstIndex << S.NumFaces << S.f24 << S.f28;
			if (Ar.ArVer >= 409) Ar << unk28;
			return Ar;
		}
#endif // MKVSDC
		if (Ar.ArVer >= 473) Ar << S.bEnableShadowCasting;
		Ar << S.FirstIndex << S.NumFaces << S.f24 << S.f28;
#if MASSEFF
		if (Ar.Game == GAME_MassEffect && Ar.ArVer >= 485)
			return Ar << S.Index;				//?? other name?
#endif // MASSEFF
#if HUXLEY
		if (Ar.Game == GAME_Huxley && Ar.ArVer >= 485)
			return Ar << S.Index;				//?? other name?
#endif // HUXLEY
		if (Ar.ArVer >= 492) Ar << S.Index;		// real version is unknown! This field is missing in GOW1_PC (490), but present in UT3 (512)
#if ALPHA_PR
		if (Ar.Game == GAME_AlphaProtocol && Ar.ArLicenseeVer >= 13)
		{
			int unk30, unk34;
			Ar << unk30 << unk34;
		}
#endif // ALPHA_PR
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 49)
		{
			TArray<FTRStaticMeshSectionUnk> f30;
			Ar << f30;
			return Ar;
		}
#endif // TRANSFORMERS
#if FABLE
		if (Ar.Game == GAME_Fable && Ar.ArLicenseeVer >= 1007)
		{
			int unk40;
			Ar << unk40;
		}
#endif // FABLE
		if (Ar.ArVer >= 514) Ar << S.f30;
#if ALPHA_PR
		if (Ar.Game == GAME_AlphaProtocol && Ar.ArLicenseeVer >= 39)
		{
			int unk38;
			Ar << unk38;
		}
#endif // ALPHA_PR
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArVer >= 575)
		{
			byte flag;
			FMOHStaticMeshSectionUnk unk3C;
			Ar << flag;
			if (flag) Ar << unk3C;
		}
#endif // MOH2010
#if BATMAN
		if ((Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3) && Ar.ArLicenseeVer >= 28)
		{
			UObject *unk4;
			Ar << unk4;
		}
#endif // BATMAN
#if BIOSHOCK3
		if (Ar.Game == GAME_Bioshock3)
		{
			FString unk4;
			Ar << unk4;
		}
#endif // BIOSHOCK3
		if (Ar.ArVer >= 618)
		{
			byte unk;
			Ar << unk;
			assert(unk == 0);
		}
#if XCOM_BUREAU
		if (Ar.Game == GAME_XcomB)
		{
			FString unk;
			Ar << unk;
		}
#endif // XCOM_BUREAU
		return Ar;
		unguard;
	}
};

struct FStaticMeshVertexStream3
{
	int					VertexSize;		// 0xC
	int					NumVerts;		// == Verts.Num()
	TArray<FVector>		Verts;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshVertexStream3 &S)
	{
		guard(FStaticMeshVertexStream3<<);

#if BATMAN
		if (Ar.Game == GAME_Batman || Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3)
		{
			byte VertexType = 0;		// appeared in Batman 2; 0 -> FVector, 1 -> half[3], 2 -> half[4] (not checked!)
			int unk18;					// default is 1
			if (Ar.ArLicenseeVer >= 41)
				Ar << VertexType;
			Ar << S.VertexSize << S.NumVerts;
			if (Ar.ArLicenseeVer >= 17)
				Ar << unk18;
	#if DEBUG_STATICMESH
			appPrintf("Batman StaticMesh VertexStream: IS:%d NV:%d VT:%d unk:%d\n", S.VertexSize, S.NumVerts, VertexType, unk18);
	#endif
			switch (VertexType)
			{
				case 0:
					Ar << RAW_ARRAY(S.Verts);
					break;
				default:
					appError("unsupported vertex type %d", VertexType);
			}
			if (Ar.ArLicenseeVer >= 24)
			{
				TArray<FMeshUVFloat> unk28;	// unknown type, TArray<dword,dword>
				Ar << unk28;
			}
			return Ar;
		}
#endif // BATMAN

		Ar << S.VertexSize << S.NumVerts;
#if DEBUG_STATICMESH
		appPrintf("StaticMesh Vertex stream: IS:%d NV:%d\n", S.VertexSize, S.NumVerts);
#endif

#if AVA
		if (Ar.Game == GAME_AVA && Ar.ArVer >= 442)
		{
			int presence;
			Ar << presence;
			if (!presence)
			{
				appNotify("AVA: StaticMesh without vertex stream");
				return Ar;
			}
		}
#endif // AVA
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 58)
		{
			int unk28;
			Ar << unk28;
		}
#endif // MOH2010
#if SHADOWS_DAMNED
		if (Ar.Game == GAME_ShadowsDamned && Ar.ArLicenseeVer >= 26)
		{
			int unk28;
			Ar << unk28;
		}
#endif // SHADOWS_DAMNED
#if MASSEFF
		if (Ar.Game == GAME_MassEffect3 && Ar.ArLicenseeVer >= 150)
		{
			int unk28;
			Ar << unk28;
		}
#endif // MASSEFF
#if PLA
		if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
		{
			FGuid unk;
			Ar << unk;
		}
#endif // PLA
#if BIOSHOCK3
		if (Ar.Game == GAME_Bioshock3)
		{
			byte IsPacked, VectorType;		// VectorType used only when IsPacked != 0
			FVector Mins, Extents;
			Ar << IsPacked << VectorType << Mins << Extents;
	#if DEBUG_STATICMESH
			appPrintf("... Bioshock3: IsPacked=%d VectorType=%d Mins=%g %g %g Extents=%g %g %g\n",
				IsPacked, VectorType, FVECTOR_ARG(Mins), FVECTOR_ARG(Extents));
	#endif
			if (IsPacked)
			{
				if (VectorType)
				{
					TArray<FVectorIntervalFixed48Bio> Vecs16x3;
					Ar << RAW_ARRAY(Vecs16x3);
					S.Verts.Add(Vecs16x3.Num());
					for (int i = 0; i < Vecs16x3.Num(); i++)
						S.Verts[i] = Vecs16x3[i].ToVector(Mins, Extents);
					appNotify("type1 - untested");	//?? not found - not used?
				}
				else
				{
					TArray<FVectorIntervalFixed64Bio> Vecs16x4;
					Ar << RAW_ARRAY(Vecs16x4);
					S.Verts.Add(Vecs16x4.Num());
					for (int i = 0; i < Vecs16x4.Num(); i++)
						S.Verts[i] = Vecs16x4[i].ToVector(Mins, Extents);
				}
				return Ar;
			}
			// else - normal vertex stream
		}
#endif // BIOSHOCK3
#if REMEMBER_ME
		if (Ar.Game == GAME_RememberMe && Ar.ArLicenseeVer >= 18)
		{
			int UsePackedPosition, VertexType;
			FVector v1, v2;
			Ar << VertexType << UsePackedPosition;
			if (Ar.ArLicenseeVer >= 20)
				Ar << v1 << v2;
//			appPrintf("VT=%d, PP=%d, V1=%g %g %g, V2=%g %g %g\n", VertexType, UsePackedPosition, FVECTOR_ARG(v1), FVECTOR_ARG(v2));
			if (UsePackedPosition && VertexType != 0)
			{
				appError("Unsupported vertex type!\n");
			}
		}
#endif // REMEMBER_ME
#if THIEF4
		if (Ar.Game == GAME_Thief4 && Ar.ArLicenseeVer >= 15)
		{
			int unk28;
			Ar << unk28;
		}
#endif
		Ar << RAW_ARRAY(S.Verts);
		return Ar;

		unguard;
	}
};


static int  GNumStaticUVSets    = 1;
static bool GUseStaticFloatUVs  = true;
static bool GStripStaticNormals = false;

struct FStaticMeshUVItem3
{
	FVector				Pos;			// old version (< 472)
	FPackedNormal		Normal[3];
	int					f10;			//?? VertexColor?
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUVItem3 &V)
	{
		guard(FStaticMeshUVItem3<<);

#if MKVSDC
		if (Ar.Game == GAME_MK)
		{
			if (Ar.ArVer >= 472) goto uvs;	// normals are stored in FStaticMeshNormalStream_MK
			int unk;
			Ar << unk;
			goto new_ver;
		}
#endif // MKVSDC
#if A51
		if (Ar.Game == GAME_A51)
			goto new_ver;
#endif
#if AVA
		if (Ar.Game == GAME_AVA)
		{
			assert(Ar.ArVer >= 441);
			Ar << V.Normal[0] << V.Normal[2] << V.f10;
			goto uvs;
		}
#endif // AVA

		if (GStripStaticNormals) goto uvs;

		if (Ar.ArVer < 472)
		{
			// old version has position embedded into UVStream (this is not an UVStream, this is a single stream for everything)
			int unk10;					// pad or color ?
			Ar << V.Pos << unk10;
		}
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 7)
		{
			int fC;
			Ar << fC;					// really should be serialized before unk10 above (it's FColor)
		}
#endif // FURY
	new_ver:
		if (Ar.ArVer < 477)
			Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
		else
			Ar << V.Normal[0] << V.Normal[2];
#if APB
		if (Ar.Game == GAME_APB && Ar.ArLicenseeVer >= 12) goto uvs;
#endif
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 58) goto uvs;
#endif
#if UNDERTOW
		if (Ar.Game == GAME_Undertow) goto uvs;
#endif
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 181) goto uvs; // Transformers: Fall of Cybertron, no version in code
#endif
		if (Ar.ArVer >= 434 && Ar.ArVer < 615)
			Ar << V.f10;				// starting from 615 made as separate stream
	uvs:
		if (GUseStaticFloatUVs)
		{
			for (int i = 0; i < GNumStaticUVSets; i++)
				Ar << V.UV[i];
		}
		else
		{
			for (int i = 0; i < GNumStaticUVSets; i++)
			{
				// read in half format and convert to float
				FMeshUVHalf UVHalf;
				Ar << UVHalf;
				V.UV[i] = UVHalf;		// convert
			}
		}
		return Ar;

		unguard;
	}
};

struct FStaticMeshUVStream3
{
	int					NumTexCoords;
	int					ItemSize;
	int					NumVerts;
	int					bUseFullPrecisionUVs;
	TArray<FStaticMeshUVItem3> UV;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUVStream3 &S)
	{
		guard(FStaticMeshUVStream3<<);

		Ar << S.NumTexCoords << S.ItemSize << S.NumVerts;
		S.bUseFullPrecisionUVs = true;
#if TUROK
		if (Ar.Game == GAME_Turok && Ar.ArLicenseeVer >= 59)
		{
			int HalfPrecision, unk28;
			Ar << HalfPrecision << unk28;
			S.bUseFullPrecisionUVs = !HalfPrecision;
			assert(S.bUseFullPrecisionUVs);
		}
#endif // TUROK
#if AVA
		if (Ar.Game == GAME_AVA && Ar.ArVer >= 441) goto new_ver;
#endif
#if MOHA
		if (Ar.Game == GAME_MOHA && Ar.ArVer >= 421) goto new_ver;
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK) goto old_ver;	// Injustice has no float/half selection
#endif
		if (Ar.ArVer >= 474)
		{
		new_ver:
			Ar << S.bUseFullPrecisionUVs;
		}
	old_ver:

#if BATMAN
		int HasNormals = 1;
		if ((Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3) && Ar.ArLicenseeVer >= 42)
			Ar << HasNormals;
		GStripStaticNormals = (HasNormals == 0);
#endif // BATMAN
#if MASSEFF
		if (Ar.Game == GAME_MassEffect3 && Ar.ArLicenseeVer >= 150)
		{
			int unk30;
			Ar << unk30;
		}
#endif // MASSEFF
#if DEBUG_STATICMESH
		appPrintf("StaticMesh UV stream: TC:%d IS:%d NV:%d FloatUV:%d\n", S.NumTexCoords, S.ItemSize, S.NumVerts, S.bUseFullPrecisionUVs);
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK)
			S.bUseFullPrecisionUVs = false;
#endif
#if A51
		if (Ar.Game == GAME_A51 && Ar.ArLicenseeVer >= 22) // or MidwayVer ?
			Ar << S.bUseFullPrecisionUVs;
#endif
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 58)
		{
			int unk30;
			Ar << unk30;
		}
#endif // MOH2010
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 34)
		{
			int unused;			// useless stack variable, always 0
			Ar << unused;
		}
#endif // FURY
#if SHADOWS_DAMNED
		if (Ar.Game == GAME_ShadowsDamned && Ar.ArLicenseeVer >= 22)
		{
			int unk30;
			Ar << unk30;
		}
#endif // SHADOWS_DAMNED
#if PLA
		if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
		{
			FGuid unk;
			Ar << unk;
		}
#endif // PLA
#if THIEF4
		if (Ar.Game == GAME_Thief4 && Ar.ArLicenseeVer >= 15)
		{
			int unk30;
			Ar << unk30;
		}
#endif
		// prepare for UV serialization
		if (S.NumTexCoords > NUM_MESH_UV_SETS)
			appError("StaticMesh has %d UV sets", S.NumTexCoords);
		GNumStaticUVSets   = S.NumTexCoords;
		GUseStaticFloatUVs = S.bUseFullPrecisionUVs;
		Ar << RAW_ARRAY(S.UV);
		return Ar;

		unguard;
	}
};

struct FStaticMeshColorStream3
{
	int					ItemSize;
	int					NumVerts;
	TArray<int>			Colors;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshColorStream3 &S)
	{
		guard(FStaticMeshColorStream3<<);
		Ar << S.ItemSize << S.NumVerts;
#if DEBUG_STATICMESH
		appPrintf("StaticMesh ColorStream: IS:%d NV:%d\n", S.ItemSize, S.NumVerts);
#endif
		return Ar << RAW_ARRAY(S.Colors);
		unguard;
	}
};

// new color stream: difference is that data array is not serialized when NumVerts is 0
struct FStaticMeshColorStream3New		// ArVer >= 615
{
	int					ItemSize;
	int					NumVerts;
	TArray<int>			Colors;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshColorStream3New &S)
	{
		guard(FStaticMeshColorStream3New<<);
		Ar << S.ItemSize << S.NumVerts;
#if DEBUG_STATICMESH
		appPrintf("StaticMesh ColorStreamNew: IS:%d NV:%d\n", S.ItemSize, S.NumVerts);
#endif
#if THIEF4
		if (Ar.Game == GAME_Thief4 && Ar.ArLicenseeVer >= 43)
		{
			byte unk;
			Ar << unk;
		}
#endif
		if (S.NumVerts)
		{
#if PLA
			if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
			{
				FGuid unk;
				Ar << unk;
			}
#endif // PLA
			Ar << RAW_ARRAY(S.Colors);
		}
		return Ar;
		unguard;
	}
};

struct FStaticMeshVertex3Old			// ArVer < 333
{
	FVector				Pos;
	FPackedNormal		Normal[3];		// packed vector

	operator FVector() const
	{
		return Pos;
	}

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshVertex3Old &V)
	{
		return Ar << V.Pos << V.Normal[0] << V.Normal[1] << V.Normal[2];
	}
};

struct FStaticMeshUVStream3Old			// ArVer < 364; corresponds to UE2 StaticMesh?
{
	TArray<FMeshUVFloat> Data;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUVStream3Old &S)
	{
		guard(FStaticMeshUVStream3Old<<);
		int unk;						// Revision?
		Ar << S.Data;					// used RAW_ARRAY, but RAW_ARRAY is newer than this version
		if (Ar.ArVer < 297) Ar << unk;
		return Ar;
		unguard;
	}
};

#if MKVSDC

struct FStaticMeshNormal_MK
{
	FPackedNormal		Normal[3];		// packed vector

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshNormal_MK &V)
	{
		if (Ar.ArVer >= 573)
			return Ar << V.Normal[0] << V.Normal[2];	// Injustice
		return Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
	}
};

struct FStaticMeshNormalStream_MK
{
	int					ItemSize;
	int					NumVerts;
	TArray<FStaticMeshNormal_MK> Normals;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshNormalStream_MK &S)
	{
		Ar << S.ItemSize << S.NumVerts << RAW_ARRAY(S.Normals);
#if DEBUG_STATICMESH
		appPrintf("MK NormalStream: ItemSize=%d, Count=%d (%d)\n", S.ItemSize, S.NumVerts, S.Normals.Num());
#endif
		return Ar;
	}
};

#endif // MKVSDC

struct FStaticMeshLODModel
{
	FByteBulkData		BulkData;		// ElementSize = 0xFC for UT3 and 0x170 for UDK ... it's simpler to skip it
	TArray<FStaticMeshSection3> Sections;
	FStaticMeshVertexStream3    VertexStream;
	FStaticMeshUVStream3        UVStream;
	FStaticMeshColorStream3     ColorStream;	//??
	FStaticMeshColorStream3New  ColorStream2;	//??
	FIndexBuffer3		Indices;
	FIndexBuffer3		Indices2;		// wireframe
	int					f80;
	TArray<FEdge3>		Edges;
	TArray<byte>		fEC;			// flags for faces? removed simultaneously with Edges

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshLODModel &Lod)
	{
		guard(FStaticMeshLODModel<<);

#if DEBUG_STATICMESH
		appPrintf("Serialize UStaticMesh LOD\n");
#endif
#if FURY
		if (Ar.Game == GAME_Fury)
		{
			int EndArPos, unkD0;
			if (Ar.ArLicenseeVer >= 8)	Ar << EndArPos;			// TLazyArray-like file pointer
			if (Ar.ArLicenseeVer >= 18)	Ar << unkD0;
		}
#endif // FURY
#if HUXLEY
		if (Ar.Game == GAME_Huxley && Ar.ArLicenseeVer >= 14)
		{
			// Huxley has different IndirectArray layout: each item
			// has stored data size before data itself
			int DataSize;
			Ar << DataSize;
		}
#endif // HUXLEY

#if APB
		if (Ar.Game == GAME_APB)
		{
			// skip bulk; check UTexture3::Serialize() for details
			Ar.Seek(Ar.Tell() + 8);
			goto after_bulk;
		}
#endif // APB
		if (Ar.ArVer >= 218)
			Lod.BulkData.Skip(Ar);

	after_bulk:

#if TLR
		if (Ar.Game == GAME_TLR && Ar.ArLicenseeVer >= 2)
		{
			FByteBulkData unk128;
			unk128.Skip(Ar);
		}
#endif // TLR
		Ar << Lod.Sections;
#if DEBUG_STATICMESH
		appPrintf("%d sections\n", Lod.Sections.Num());
		for (int i = 0; i < Lod.Sections.Num(); i++)
		{
			FStaticMeshSection3 &S = Lod.Sections[i];
			appPrintf("Mat: %s\n", S.Mat ? S.Mat->Name : "?");
			appPrintf("  %d %d sh=%d i0=%d NF=%d %d %d idx=%d\n", S.f10, S.f14, S.bEnableShadowCasting, S.FirstIndex, S.NumFaces, S.f24, S.f28, S.Index);
		}
#endif // DEBUG_STATICMESH
		// serialize vertex and uv streams
#if A51
		if (Ar.Game == GAME_A51) goto new_ver;
#endif
#if MKVSDC || AVA
		if (Ar.Game == GAME_MK || Ar.Game == GAME_AVA) goto ver_3;
#endif

#if BORDERLANDS
		if (Ar.Game == GAME_Borderlands && Ar.ArLicenseeVer >= 57 && Ar.ArVer < 832)	// Borderlands 1, version unknown; not valid for Borderlands 2
		{
			// refined field set
			Ar << Lod.VertexStream;
			Ar << Lod.UVStream;
			Ar << Lod.f80;
			Ar << Lod.Indices;
			// note: no fEC (smoothing groups?)
			return Ar;
		}
#endif // BORDERLANDS

#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers)
		{
			// code is similar to original code (ArVer >= 472) but has different versioning and a few new fields
			FTRMeshUnkStream unkStream;		// normals?
			int unkD8;						// part of Indices2
			Ar << Lod.VertexStream << Lod.UVStream;
			if (Ar.ArVer >= 516) Ar << Lod.ColorStream2;
			if (Ar.ArLicenseeVer >= 71) Ar << unkStream;
			if (Ar.ArVer < 536) Ar << Lod.ColorStream;
			Ar << Lod.f80 << Lod.Indices << Lod.Indices2;
			if (Ar.ArLicenseeVer >= 58) Ar << unkD8;
			if (Ar.ArLicenseeVer >= 181)	// Fall of Cybertron
			{								// pre-Fall of Cybertron has 0 or 1 ints after indices, but Fall of Cybertron has 1 or 2 ints
				int unkIndexStreamField;
				Ar << unkIndexStreamField;
			}
			if (Ar.ArVer < 536)
			{
				Ar << RAW_ARRAY(Lod.Edges);
				Ar << Lod.fEC;
			}
			return Ar;
		}
#endif // TRANSFORMERS

		if (Ar.ArVer >= 472)
		{
		new_ver:
			Ar << Lod.VertexStream;
			Ar << Lod.UVStream;
#if MOH2010
			if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 55) goto color_stream;
#endif
#if BLADENSOUL
			if (Ar.Game == GAME_BladeNSoul && Ar.ArVer >= 572) goto color_stream;
#endif
			// unknown data in UDK
			if (Ar.ArVer >= 615)
			{
			color_stream:
				Ar << Lod.ColorStream2;
			}
			if (Ar.ArVer < 686) Ar << Lod.ColorStream;	//?? probably this is not a color stream - the same version is used to remove "edges"
			Ar << Lod.f80;
		}
		else if (Ar.ArVer >= 466)
		{
		ver_3:
#if MKVSDC
			if (Ar.Game == GAME_MK && Ar.ArVer >= 472) // MK9; real version: MidwayVer >= 36
			{
				FStaticMeshNormalStream_MK NormalStream;
				Ar << Lod.VertexStream << Lod.ColorStream << NormalStream << Lod.UVStream << Lod.f80;
				// copy NormalStream into UVStream
				assert(Lod.UVStream.UV.Num() == NormalStream.Normals.Num());
				for (int i = 0; i < Lod.UVStream.UV.Num(); i++)
				{
					FStaticMeshUVItem3  &UV = Lod.UVStream.UV[i];
					FStaticMeshNormal_MK &N = NormalStream.Normals[i];
					UV.Normal[0] = N.Normal[0];
					UV.Normal[1] = N.Normal[1];
					UV.Normal[2] = N.Normal[2];
				}
				goto duplicate_verts;
			}
#endif // MKVSDC
			Ar << Lod.VertexStream;
			Ar << Lod.UVStream;
			Ar << Lod.f80;
#if MKVSDC || AVA
			if (Ar.Game == GAME_MK || Ar.Game == GAME_AVA)
			{
			duplicate_verts:
				// note: sometimes UVStream has 2 times more items than VertexStream
				// we should duplicate vertices
				int n1 = Lod.VertexStream.Verts.Num();
				int n2 = Lod.UVStream.UV.Num();
				if (n1 * 2 == n2)
				{
					appPrintf("Duplicating MK StaticMesh verts\n");
					Lod.VertexStream.Verts.Add(n1);
					for (int i = 0; i < n1; i++)
						Lod.VertexStream.Verts[i+n1] = Lod.VertexStream.Verts[i];
				}
			}
#endif // MKVSDC || AVA
		}
		else if (Ar.ArVer >= 364)
		{
		// ver_2:
			Ar << Lod.UVStream;
			Ar << Lod.f80;
			// create VertexStream
			int NumVerts = Lod.UVStream.UV.Num();
			Lod.VertexStream.Verts.Empty(NumVerts);
//			Lod.VertexStream.NumVerts = NumVerts;
			for (int i = 0; i < NumVerts; i++)
				Lod.VertexStream.Verts.AddItem(Lod.UVStream.UV[i].Pos);
		}
		else
		{
		// ver_1:
			TArray<FStaticMeshUVStream3Old> UVStream;
			if (Ar.ArVer >= 333)
			{
				appNotify("StaticMesh: untested code! (ArVer=%d)", Ar.ArVer);
				TArray<FQuat> Verts;
				TArray<int>   Normals;	// compressed
				Ar << Verts << Normals << UVStream;	// really used RAW_ARRAY, but it is too new for this code
				//!! convert
			}
			else
			{
				// oldest version
				TArray<FStaticMeshVertex3Old> Verts;
				Ar << Verts << UVStream;
				// convert vertex stream
				int i;
				int NumVerts     = Verts.Num();
				int NumTexCoords = UVStream.Num();
				if (NumTexCoords > NUM_MESH_UV_SETS)
				{
					appNotify("StaticMesh has %d UV sets", NumTexCoords);
					NumTexCoords = NUM_MESH_UV_SETS;
				}
				Lod.VertexStream.Verts.Empty(NumVerts);
				Lod.VertexStream.Verts.Add(NumVerts);
				Lod.UVStream.UV.Empty();
				Lod.UVStream.UV.Add(NumVerts);
				Lod.UVStream.NumVerts     = NumVerts;
				Lod.UVStream.NumTexCoords = NumTexCoords;
				// resize UV streams
				for (i = 0; i < NumVerts; i++)
				{
					FStaticMeshVertex3Old &V = Verts[i];
					FVector              &DV = Lod.VertexStream.Verts[i];
					FStaticMeshUVItem3   &UV = Lod.UVStream.UV[i];
					DV           = V.Pos;
					UV.Normal[2] = V.Normal[2];
					for (int j = 0; j < NumTexCoords; j++)
						UV.UV[j] = UVStream[j].Data[i];
				}
			}
		}
	indices:
#if DEBUG_STATICMESH
		appPrintf("Serializing indices ...\n");
#endif
#if BATMAN
		if ((Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3) && Ar.ArLicenseeVer >= 45)
		{
			int unk34;		// variable in IndexBuffer, but in 1st only
			Ar << unk34;
		}
#endif // BATMAN
#if PLA
		if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
		{
			FGuid unk;
			Ar << unk;
		}
#endif // PLA
		Ar << Lod.Indices;
#if ENDWAR
		if (Ar.Game == GAME_EndWar) goto after_indices;	// single Indices buffer since version 262
#endif
#if APB
		if (Ar.Game == GAME_APB)
		{
			// serialized FIndexBuffer3 guarded by APB bulk seeker (check UTexture3::Serialize() for details)
			Ar.Seek(Ar.Tell() + 8);
			goto after_indices;				// do not need this data
		}
#endif // APB
#if BORDERLANDS
		if (Ar.Game == GAME_Borderlands && Ar.ArVer >= 832) goto after_indices; // Borderlands 2
#endif
		Ar << Lod.Indices2;
	after_indices:

		if (Ar.ArVer < 686)
		{
			Ar << RAW_ARRAY(Lod.Edges);
			Ar << Lod.fEC;
		}
#if ALPHA_PR
		if (Ar.Game == GAME_AlphaProtocol)
		{
			assert(Ar.ArLicenseeVer > 8);	// ArLicenseeVer = [1..7] has custom code
			if (Ar.ArLicenseeVer >= 4)
			{
				TArray<int> unk128;
				Ar << RAW_ARRAY(unk128);
			}
		}
#endif // ALPHA_PR
#if AVA
		if (Ar.Game == GAME_AVA)
		{
			if (Ar.ArLicenseeVer >= 2)
			{
				int fFC, f100;
				Ar << fFC << f100;
			}
			if (Ar.ArLicenseeVer >= 4)
			{
				FByteBulkData f104, f134, f164, f194, f1C4, f1F4, f224, f254;
				f104.Skip(Ar);
				f134.Skip(Ar);
				f164.Skip(Ar);
				f194.Skip(Ar);
				f1C4.Skip(Ar);
				f1F4.Skip(Ar);
				f224.Skip(Ar);
				f254.Skip(Ar);
			}
		}
#endif // AVA

		if (Ar.ArVer >= 841)
		{
			FIndexBuffer3 Indices3;
#if PLA
			if (Ar.Game == GAME_PLA && Ar.ArVer >= 900)
			{
				FGuid unk;
				Ar << unk;
			}
#endif // PLA
			Ar << Indices3;
			if (Indices3.Indices.Num())
				appPrintf("LOD has extra index buffer (%d items)\n", Indices3.Indices.Num());
		}

		return Ar;

		unguard;
	}
};

struct FkDOPBounds		// bounds for compressed (quantized) kDOP node
{
	FVector				v1;
	FVector				v2;

	friend FArchive& operator<<(FArchive &Ar, FkDOPBounds &V)
	{
#if ENSLAVED
		if (Ar.Game == GAME_Enslaved)
		{
			// compressed structure
			short v1[3], v2[3];
			Ar << v1[0] << v1[1] << v1[2] << v2[0] << v2[1] << v2[2];
			return Ar;
		}
#endif // ENSLAVED
		return Ar << V.v1 << V.v2;
	}
};

struct FkDOPNode3
{
	FkDOPBounds			Bounds;
	int					f18;
	short				f1C;
	short				f1E;

	friend FArchive& operator<<(FArchive &Ar, FkDOPNode3 &V)
	{
#if ENSLAVED
		if (Ar.Game == GAME_Enslaved)
		{
			// all data compressed
			byte  fC, fD;
			short fE;
			Ar << V.Bounds;		// compressed
			Ar << fC << fD << fE;
			return Ar;
		}
#endif // ENSLAVED
#if DCU_ONLINE
		if (Ar.Game == GAME_DCUniverse && (Ar.ArLicenseeVer & 0xFF00) >= 0xA00)
			return Ar << V.f18 << V.f1C << V.f1E;	// no Bounds field - global for all nodes
#endif // DCU_ONLINE
		Ar << V.Bounds << V.f18;
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 7) goto new_ver;
#endif
#if MOHA
		if (Ar.Game == GAME_MOHA && Ar.ArLicenseeVer >= 8) goto new_ver;
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK) goto old_ver;
#endif
		if ((Ar.ArVer < 209) || (Ar.ArVer >= 468))
		{
		new_ver:
			Ar << V.f1C << V.f1E;	// short
		}
		else
		{
		old_ver:
			// old version
			assert(Ar.IsLoading);
			int tmp1C, tmp1E;
			Ar << tmp1C << tmp1E;
			V.f1C = tmp1C;
			V.f1E = tmp1E;
		}
		return Ar;
	}
};

struct FkDOPNode3New	// starting from version 770
{
	byte				mins[3];
	byte				maxs[3];

	friend FArchive& operator<<(FArchive &Ar, FkDOPNode3New &V)
	{
		Ar << V.mins[0] << V.mins[1] << V.mins[2] << V.maxs[0] << V.maxs[1] << V.maxs[2];
		return Ar;
	}
};

SIMPLE_TYPE(FkDOPNode3New, byte)


struct FkDOPTriangle3
{
	short				f0, f2, f4, f6;

	friend FArchive& operator<<(FArchive &Ar, FkDOPTriangle3 &V)
	{
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 25) goto new_ver;
#endif
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 7) goto new_ver;
#endif
#if MOHA
		if (Ar.Game == GAME_MOHA && Ar.ArLicenseeVer >= 8) goto new_ver;
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK) goto old_ver;
#endif
		if ((Ar.ArVer < 209) || (Ar.ArVer >= 468))
		{
		new_ver:
			Ar << V.f0 << V.f2 << V.f4 << V.f6;
		}
		else
		{
		old_ver:
			assert(Ar.IsLoading);
			int tmp0, tmp2, tmp4, tmp6;
			Ar << tmp0 << tmp2 << tmp4 << tmp6;
			V.f0 = tmp0;
			V.f2 = tmp2;
			V.f4 = tmp4;
			V.f6 = tmp6;
		}
		return Ar;
	}
};


#if FURY

struct FFuryStaticMeshUnk	// in other ganes this structure serialized after LOD models, in Fury - before
{
	int					unk0;
	int					fC, f10, f14;
	TArray<short>		f18;				// old version uses TArray<int>, new - TArray<short>, but there is no code selection
											// (array size in old version is always 0?)

	friend FArchive& operator<<(FArchive &Ar, FFuryStaticMeshUnk &S)
	{
		if (Ar.ArVer < 297) Ar << S.unk0;	// Version? (like in FIndexBuffer3)
		if (Ar.ArLicenseeVer >= 4)			// Fury-specific
			Ar << S.fC << S.f10 << S.f14 << S.f18;
		return Ar;
	}
};

#endif // FURY


struct FStaticMeshUnk5
{
	int					f0;
	byte				f4[3];

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUnk5 &S)
	{
		return Ar << S.f0 << S.f4[0] << S.f4[1] << S.f4[2];
	}
};


void UStaticMesh3::Serialize(FArchive &Ar)
{
	guard(UStaticMesh3::Serialize);

	Super::Serialize(Ar);

#if FURY
	if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 14)
	{
		int unk3C, unk40;
		Ar << unk3C << unk40;
	}
#endif // FURY
#if DARKVOID
	if (Ar.Game == GAME_DarkVoid)
	{
		int unk180, unk18C, unk198;
		if (Ar.ArLicenseeVer >= 5) Ar << unk180 << unk18C;
		if (Ar.ArLicenseeVer >= 6) Ar << unk198;
	}
#endif // DARKVOID
#if TERA
	if (Ar.Game == GAME_Tera && Ar.ArLicenseeVer >= 3)
	{
		FString SourceFileName;
		Ar << SourceFileName;
	}
#endif // TERA
#if TRANSFORMERS
	if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 50)
	{
		Ar << RAW_ARRAY(kDOPNodes) << RAW_ARRAY(kDOPTriangles) << Lods;
		// note: Bounds is serialized as property (see UStaticMesh in h-file)
		goto done;
	}
#endif // TRANSFORMERS

	Ar << Bounds << BodySetup;
#if TUROK
	if (Ar.Game == GAME_Turok && Ar.ArLicenseeVer >= 59)
	{
		int unkFC, unk100;
		Ar << unkFC << unk100;
	}
#endif // TUROK
	if (Ar.ArVer < 315)
	{
		UObject *unk;
		Ar << unk;
	}
#if ENDWAR
	if (Ar.Game == GAME_EndWar) goto version;	// no kDOP since version 306
#endif // ENDWAR
#if SINGULARITY
	if (Ar.Game == GAME_Singularity)
	{
		// serialize kDOP tree
		assert(Ar.ArLicenseeVer >= 112);
		// old serialization code
		Ar << RAW_ARRAY(kDOPNodes) << RAW_ARRAY(kDOPTriangles);
		// new serialization code
		// bug in Singularity serialization code: serialized the same things twice!
		goto new_kdop;
	}
#endif // SINGULARITY
#if BULLETSTORM
	if (Ar.Game == GAME_Bulletstorm && Ar.ArVer >= 739) goto new_kdop;
#endif
#if MASSEFF
	if (Ar.Game == GAME_MassEffect3 && Ar.ArLicenseeVer >= 153) goto new_kdop;
#endif
#if DISHONORED
	if (Ar.Game == GAME_Dishonored) goto old_kdop;
#endif
#if BIOSHOCK3
	if (Ar.Game == GAME_Bioshock3)
	{
		FVector v1, v2[2], v3;
		TArray<int> arr4;
		Ar << v1 << v2[0] << v2[1] << v3 << arr4;
		goto version;
	}
#endif
#if THIEF4
	if (Ar.Game == GAME_Thief4 && Ar.ArVer >= 707) goto new_kdop;
#endif
	// kDOP tree
	if (Ar.ArVer < 770)
	{
	old_kdop:
		Ar << RAW_ARRAY(kDOPNodes);
	}
	else
	{
	new_kdop:
		FkDOPBounds Bounds;
		TArray<FkDOPNode3New> Nodes;
		Ar << Bounds << RAW_ARRAY(Nodes);
	}
#if FURY
	if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 32)
	{
		int kDopUnk;
		Ar << kDopUnk;
	}
#endif // FURY
	Ar << RAW_ARRAY(kDOPTriangles);
#if DCU_ONLINE
	if (Ar.Game == GAME_DCUniverse && (Ar.ArLicenseeVer & 0xFF00) >= 0xA00)
	{
		// this game stored kDOP bounds only once
		FkDOPBounds Bounds;
		Ar << Bounds;
	}
#endif // DCU_ONLINE
#if DOH
	if (Ar.Game == GAME_DOH && Ar.ArLicenseeVer >= 73)
	{
		FVector			unk18;		// extra computed kDOP field
		TArray<FVector>	unkA0;
		int				unk74;
		Ar << unk18;
		Ar << InternalVersion;		// has InternalVersion = 0x2000F
		Ar << unkA0 << unk74 << Lods;
		goto done;
	}
#endif // DOH
#if THIEF4
	if (Ar.Game == GAME_Thief4)
	{
		if (Ar.ArLicenseeVer >= 7)
			SkipFixedArray(Ar, sizeof(int) * 7);
		if (Ar.ArLicenseeVer >= 5)
			SkipFixedArray(Ar, sizeof(int) * (2+9));	// FName + int[9]
		if (Ar.ArLicenseeVer >= 3)
			SkipFixedArray(Ar, sizeof(int) * 7);
		if (Ar.ArLicenseeVer >= 2)
		{
			SkipFixedArray(Ar, sizeof(int) * 7);
			SkipFixedArray(Ar, sizeof(int) * 10);
			// complex structure
			SkipFixedArray(Ar, sizeof(int) * 3);
			SkipFixedArray(Ar, sizeof(int));
			SkipFixedArray(Ar, sizeof(int));
		}
	}
#endif // THIEF4

version:
	Ar << InternalVersion;

#if DEBUG_STATICMESH
	appPrintf("kDOPNodes=%d kDOPTriangles=%d\n", kDOPNodes.Num(), kDOPTriangles.Num());
	appPrintf("ver: %d\n", InternalVersion);
#endif

#if FURY
	if (Ar.Game == GAME_Fury)
	{
		int unk1, unk2;
		TArray<FFuryStaticMeshUnk> unk50;
		if (Ar.ArLicenseeVer >= 34) Ar << unk1;
		if (Ar.ArLicenseeVer >= 33) Ar << unk2;
		if (Ar.ArLicenseeVer >= 8)  Ar << unk50;
		InternalVersion = 16;		// uses InternalVersion=18
	}
#endif // FURY
#if TRANSFORMERS
	if (Ar.Game == GAME_Transformers) goto lods;	// The Bourne Conspiracy has InternalVersion=17
#endif

	if (InternalVersion >= 17 && Ar.ArVer < 593)
	{
		TArray<FName> unk;			// some text properties; ContentTags ? (switched from binary to properties)
		Ar << unk;
	}
	if (Ar.ArVer >= 823)
	{
		guard(SerializeExtraLOD);

		int unkFlag;
		FStaticMeshLODModel unkLod;
		Ar << unkFlag;
		if (unkFlag)
		{
			appPrintf("has extra LOD model\n");
			Ar << unkLod;
		}

		if (Ar.ArVer < 829)
		{
			TArray<int> unk;
			Ar << unk;
		}
		else
		{
			TArray<FStaticMeshUnk5> f178;
			Ar << f178;
		}
		int f74;
		Ar << f74;

		unguard;
	}
#if SHADOWS_DAMNED
	if (Ar.Game == GAME_ShadowsDamned && Ar.ArLicenseeVer >= 26)
	{
		int unk134;
		Ar << unk134;
	}
#endif // SHADOWS_DAMNED

	if (Ar.ArVer >= 859)
	{
		int unk;
		Ar << unk;
	}

lods:
	Ar << Lods;

//	Ar << f48;

done:
	DROP_REMAINING_DATA(Ar);

	// convert UStaticMesh3 to CStaticMesh

	guard(ConvertMesh);

	CStaticMesh *Mesh = new CStaticMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;		//?? UE3 meshes has radius 2 times larger than mesh
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// convert lods
	Mesh->Lods.Empty(Lods.Num());
	for (int lod = 0; lod < Lods.Num(); lod++)
	{
		guard(ConvertLod);

		const FStaticMeshLODModel &SrcLod = Lods[lod];
		CStaticMeshLod *Lod = new (Mesh->Lods) CStaticMeshLod;

		int NumTexCoords = SrcLod.UVStream.NumTexCoords;
		int NumVerts     = SrcLod.VertexStream.Verts.Num();

		Lod->NumTexCoords = NumTexCoords;
		Lod->HasNormals   = true;
		Lod->HasTangents  = (Ar.ArVer >= 364);				//?? check; FStaticMeshUVStream3 is used since this version
#if BATMAN
		if ((Ar.Game == GAME_Batman2 || Ar.Game == GAME_Batman3) && CanStripNormalsAndTangents)
			Lod->HasNormals = Lod->HasTangents = false;
#endif
		if (NumTexCoords > NUM_MESH_UV_SETS)
			appError("StaticMesh has %d UV sets", NumTexCoords);

		// sections
		Lod->Sections.Add(SrcLod.Sections.Num());
		for (int i = 0; i < SrcLod.Sections.Num(); i++)
		{
			CMeshSection &Dst = Lod->Sections[i];
			const FStaticMeshSection3 &Src = SrcLod.Sections[i];
			Dst.Material   = Src.Mat;
			Dst.FirstIndex = Src.FirstIndex;
			Dst.NumFaces   = Src.NumFaces;
		}

		// vertices
		Lod->AllocateVerts(NumVerts);
		for (int i = 0; i < NumVerts; i++)
		{
			const FStaticMeshUVItem3 &SUV = SrcLod.UVStream.UV[i];
			CStaticMeshVertex &V = Lod->Verts[i];

			V.Position = CVT(SrcLod.VertexStream.Verts[i]);
			UnpackNormals(SUV.Normal, V);
			// copy UV
			staticAssert((sizeof(CMeshUVFloat) == sizeof(FMeshUVFloat)) && (sizeof(V.UV) == sizeof(SUV.UV)), Incompatible_CStaticMeshUV);
#if 0
			for (int j = 0; j < NumTexCoords; j++)
				V.UV[j] = (CMeshUVFloat&/SUV.UV[j];
#else
			memcpy(V.UV, SUV.UV, sizeof(V.UV));
#endif
			//!! also has ColorStream
		}

		// indices
		Lod->Indices.Initialize(&SrcLod.Indices.Indices);			// 16-bit only

		unguardf(("lod=%d", lod));
	}

	Mesh->FinalizeMesh();

	unguard;	// ConvertMesh

	unguard;
}


#endif // UNREAL3