//
// Copyright 2014 ADVANCED MICRO DEVICES, INC.  All Rights Reserved.
//
// AMD is granting you permission to use this software and documentation (if
// any) (collectively, the "Materials") pursuant to the terms and conditions
// of the Software License Agreement included with the Materials.  If you do
// not have a copy of the Software License Agreement, contact your AMD
// representative for a copy.
// You agree that you will not reverse engineer or decompile the Materials,
// in whole or in part, except as allowed by applicable law.
//
// WARRANTY DISCLAIMER: THE SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND.  AMD DISCLAIMS ALL WARRANTIES, EXPRESS, IMPLIED, OR STATUTORY,
// INCLUDING BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE, NON-INFRINGEMENT, THAT THE SOFTWARE
// WILL RUN UNINTERRUPTED OR ERROR-FREE OR WARRANTIES ARISING FROM CUSTOM OF
// TRADE OR COURSE OF USAGE.  THE ENTIRE RISK ASSOCIATED WITH THE USE OF THE
// SOFTWARE IS ASSUMED BY YOU.
// Some jurisdictions do not allow the exclusion of implied warranties, so
// the above exclusion may not apply to You. 
// 
// LIMITATION OF LIABILITY AND INDEMNIFICATION:  AMD AND ITS LICENSORS WILL
// NOT, UNDER ANY CIRCUMSTANCES BE LIABLE TO YOU FOR ANY PUNITIVE, DIRECT,
// INCIDENTAL, INDIRECT, SPECIAL OR CONSEQUENTIAL DAMAGES ARISING FROM USE OF
// THE SOFTWARE OR THIS AGREEMENT EVEN IF AMD AND ITS LICENSORS HAVE BEEN
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.  
// In no event shall AMD's total liability to You for all damages, losses,
// and causes of action (whether in contract, tort (including negligence) or
// otherwise) exceed the amount of $100 USD.  You agree to defend, indemnify
// and hold harmless AMD and its licensors, and any of their directors,
// officers, employees, affiliates or agents from and against any and all
// loss, damage, liability and other expenses (including reasonable attorneys'
// fees), resulting from Your use of the Software or violation of the terms and
// conditions of this Agreement.  
//
// U.S. GOVERNMENT RESTRICTED RIGHTS: The Materials are provided with "RESTRICTED
// RIGHTS." Use, duplication, or disclosure by the Government is subject to the
// restrictions as set forth in FAR 52.227-14 and DFAR252.227-7013, et seq., or
// its successor.  Use of the Materials by the Government constitutes
// acknowledgement of AMD's proprietary rights in them.
// 
// EXPORT RESTRICTIONS: The Materials may be subject to export restrictions as
// stated in the Software License Agreement.
//
/*
struct AABBNode
{
	float3 center;
	float3 radius;
	uint index;
	uint zCode;
};

//--------------------------------------------------------------------------------------
// Structured Buffers
//--------------------------------------------------------------------------------------
RWStructuredBuffer<AABBNode> Data : register(u0);

//--------------------------------------------------------------------------------------
// Bitonic Sort Compute Shader
//--------------------------------------------------------------------------------------
cbuffer NumElementsCB : register(b0)
{
	int g_NumElements;
	int g_TreeOffset;
	int3 job_params;
};

[numthreads(256, 1, 1)]
void main(uint3 Gid	: SV_GroupID,
	uint3 GTid : SV_GroupThreadID)
{
	int4 tgp;

	tgp.x = Gid.x * 256;
	tgp.y = 0;
	tgp.z = g_NumElements;
	tgp.w = min(512, max(0, g_NumElements - Gid.x * 512));

	uint localID = tgp.x + GTid.x; // calculate threadID within this sortable-array

	uint index_low = localID & (job_params.x - 1);
	uint index_high = 2 * (localID - index_low);

	uint index = tgp.y + index_high + index_low;
	uint nSwapElem = tgp.y + index_high + job_params.y + job_params.z*index_low;

	if (nSwapElem<tgp.y + tgp.z)
	{
		AABBNode a = Data[g_TreeOffset + index];
		AABBNode b = Data[g_TreeOffset + nSwapElem];

		if (a.zCode > b.zCode)
		{
			Data[g_TreeOffset + index] = b;
			Data[g_TreeOffset + nSwapElem] = a;
		}
	}
}*/
struct AABBNode
{
	float3 center;
	float3 radius;
	uint index;
	uint zCode;
};
RWStructuredBuffer<AABBNode> Data : register(u0);

//--------------------------------------------------------------------------------------
// Bitonic Sort Compute Shader
//--------------------------------------------------------------------------------------
cbuffer NumElementsCB : register(b0)
{
	int g_NumElements;
	int treeOffset;
};

cbuffer SortConstants : register(b1)
{
	int4 job_params;
};

[numthreads(256, 1, 1)]
void main(uint3 Gid	: SV_GroupID,
	uint3 GTid : SV_GroupThreadID)
{
	int4 tgp;

	tgp.x = Gid.x * 256;
	tgp.y = 0;
	tgp.z = g_NumElements.x;
	tgp.w = min(512, max(0, g_NumElements.x - Gid.x * 512));

	uint localID = tgp.x + GTid.x; // calculate threadID within this sortable-array

	uint index_low = localID & (job_params.x - 1);
	uint index_high = 2 * (localID - index_low);

	uint index = tgp.y + index_high + index_low;
	uint nSwapElem = tgp.y + index_high + job_params.y + job_params.z*index_low;

	if (nSwapElem<tgp.y + tgp.z)
	{
		AABBNode a = Data[index + treeOffset];
		AABBNode b = Data[nSwapElem + treeOffset];

		if (a.zCode > b.zCode)
		{
			Data[index + treeOffset] = b;
			Data[nSwapElem + treeOffset] = a;
		}
	}
}