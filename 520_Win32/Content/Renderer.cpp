﻿#include "pch.h"
#include "Renderer.h"
#include "DirectXHelper.h"
#include "DDSLoader\DDSTextureLoader.h"
#include "DirectXTex\DirectXTex.h"

#include <algorithm>
#include <tuple>
#include <map>
#include <set>
#include <locale>
#include <codecvt>
#include <string>
#include <vector>

using namespace _520;

using namespace DirectX;

// Loads vertex and pixel shaders from files and instantiates the cube geometry.
Renderer::Renderer(const std::shared_ptr<DX::DeviceResources> deviceResources, std::shared_ptr<Profiler> profiler) :
	m_loadingComplete(false),
	m_deviceResources(deviceResources),
	m_camera(std::make_shared<Camera>()),
	m_profiler(profiler)
{
	ID3D11RasterizerState *rs;
	D3D11_RASTERIZER_DESC rsdesc;
	ZeroMemory(&rsdesc, sizeof(D3D11_RASTERIZER_DESC));
	rsdesc.FillMode = D3D11_FILL_SOLID;
	rsdesc.CullMode = D3D11_CULL_NONE;
	rsdesc.FrontCounterClockwise = true;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateRasterizerState(&rsdesc, &rs)
		);
	m_deviceResources->GetD3DDeviceContext()->RSSetState(rs);

	CreateWindowSizeDependentResources();
	CreateDeviceDependentResources();

	// Get the bounding box of the whole scene, in world coordinates.
	float max[3] = { -D3D11_FLOAT32_MAX, -D3D11_FLOAT32_MAX, -D3D11_FLOAT32_MAX };
	float min[3] = { D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX };
	for (const VertexPositionTextureNormal &vert : m_vertices) {
		float vertFloat[3] = { vert.pos.x, vert.pos.y, vert.pos.z };
		for (int i = 0; i < 3; i++)
		{
			min[i] = min(vertFloat[i], min[i]);
			max[i] = max(vertFloat[i], max[i]);
		}
	}

	// Minimum coordinates.
	XMMATRIX worldMat = XMMatrixTranspose(XMLoadFloat4x4(&m_constantBufferData.model));
	XMFLOAT3 minF = { min[0], min[1], min[2] };
	XMVECTOR minV = XMLoadFloat3(&minF);
	minV = XMVector3Transform(minV, worldMat);
	XMStoreFloat3(&minF, minV);
	float worldMin[3] = { minF.x, minF.y, minF.z };
	// Maximum coordinates.
	XMFLOAT3 maxF = { max[0], max[1], max[2] };
	XMVECTOR maxV = XMLoadFloat3(&maxF);
	maxV = XMVector3Transform(maxV, worldMat);
	XMStoreFloat3(&maxF, maxV);
	float worldMax[3] = { maxF.x, maxF.y, maxF.z };

	// Initialise/generate the lights.
	_Lights::init(worldMin, worldMax);

	CreateClusterResources();
	CreateLightResources();
}

void Renderer::LoadTexture(std::string name, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &view)
{
	Microsoft::WRL::ComPtr<ID3D11Resource> texture;

	std::string str = "Assets\\crytek-sponza\\" + name;
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	std::wstring wide = converter.from_bytes(str);
	const wchar_t *filename = wide.c_str();

	DX::ThrowIfFailed(
		CreateDDSTextureFromFile(m_deviceResources->GetD3DDevice(), filename, &texture, &view)
		);
}

void Renderer::LoadModels()
{
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;

	std::string err = tinyobj::LoadObj(shapes, materials, "Assets/crytek-sponza/sponza.obj", "Assets/crytek-sponza/");
	if (err != "") exit(0);

	m_nVertices = 0;
	m_nTriIndices = 0;
	for (int i = 0; i < shapes.size(); i++)
	{
		int nVertices = shapes[i].mesh.positions.size() / 3;
		int nTriIndices = shapes[i].mesh.indices.size() / 3;
		m_meshes.push_back(SimpleMesh{ m_nVertices, nVertices, m_nTriIndices, nTriIndices });
		m_nVertices += nVertices;
		m_nTriIndices += nTriIndices;
	}
	m_vertices.resize(m_nVertices);
	m_triIndices.resize(m_nTriIndices);
	for (int i = 0; i < m_meshes.size(); i++)
	{
		SimpleMesh &mesh = m_meshes[i];
		for (int j = 0; j < mesh.m_nVertices; j++)
		{
			tinyobj::mesh_t &m = shapes[i].mesh;
			XMFLOAT3 position(m.positions[3 * j], m.positions[3 * j + 1], m.positions[3 * j + 2]);
			XMFLOAT2 texCoord(m.texcoords[2 * j], m.texcoords[2 * j + 1]);
			XMFLOAT3 normal(m.normals[3 * j], m.normals[3 * j + 1], m.normals[3 * j + 2]);
			m_vertices[j + mesh.m_vertexOffset].pos = position;
			m_vertices[j + mesh.m_vertexOffset].texcoord = texCoord;
			m_vertices[j + mesh.m_vertexOffset].normal = normal;
		}
		for (int j = 0; j < mesh.m_nTriIndices; j++)
		{
			m_triIndices[j + mesh.m_triIndexOffset].index[0] = m_meshes[i].m_vertexOffset + shapes[i].mesh.indices[3 * j];
			m_triIndices[j + mesh.m_triIndexOffset].index[1] = m_meshes[i].m_vertexOffset + shapes[i].mesh.indices[3 * j + 1];
			m_triIndices[j + mesh.m_triIndexOffset].index[2] = m_meshes[i].m_vertexOffset + shapes[i].mesh.indices[3 * j + 2];
		}
	}
	
	// Make a list of Materials, each one with a list of the meshes it is to be applied to. 
	//std::map<std::string, Microsoft::WRL::ComPtr<ID3D11Resource>> diffuses;
	std::map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> diffuseViews;
	auto comp = [](std::shared_ptr<Material> a, std::shared_ptr<Material> b)->bool{
		return a->m_name < b->m_name;
	};
	std::set <std::shared_ptr<Material>, std::function<bool(std::shared_ptr<Material>, std::shared_ptr<Material>)>> materialsCache(comp);
	for (int i = 0; i < m_meshes.size(); i++)
	{
		for (auto matId : shapes[i].mesh.material_ids)
		{
			std::string &name = materials[matId].diffuse_texname;
			if (name.empty())
				continue;

			if (diffuseViews.find(name) == diffuseViews.end())
			{
				LoadTexture(name, diffuseViews[name]);
			}
			
			auto pMaterial = std::make_shared<Material>();
			pMaterial->m_name = name;
			pMaterial->m_diffuseView = diffuseViews[name];

			auto matIt = materialsCache.insert(pMaterial).first;
			if (std::find((*matIt)->m_meshIndices.begin(), (*matIt)->m_meshIndices.end(), i) ==
				(*matIt)->m_meshIndices.end())
			{
				(*matIt)->m_meshIndices.push_back(i);
			}
		}
	}
	m_materials.assign(materialsCache.begin(), materialsCache.end());
}

// Initializes view parameters when the window size changes.
void Renderer::CreateWindowSizeDependentResources()
{
	RECT windowSize = m_deviceResources->GetWindowSize();

	UINT width = windowSize.right - windowSize.left;
	UINT height = windowSize.bottom - windowSize.top;

	// Setting some constant buffers.
	m_lightScreenDimCBufferData.windowWidth = width;
	m_lightScreenDimCBufferData.windowHeight = height;

	float aspectRatio = width / (float)height;
	m_fovAngleY = 70.0f * XM_PI / 180.0f;

	// This is a simple example of change that can be made when the app is in
	// portrait or snapped view.
	if (aspectRatio < 1.0f)
	{
		m_fovAngleY *= 2.0f;
	}

	// Setting some constant buffers.
	m_assignClustersCBufferData.denominator = 1.0f / log(1 + 2 * tan(m_fovAngleY * 0.5) / ((float)height / 32.0f));

	// This sample makes use of a right-handed coordinate system using row-major matrices.
	XMMATRIX perspectiveMatrix = XMMatrixPerspectiveFovRH(
		m_fovAngleY,
		aspectRatio,
		0.01f,
		100.0f
		);

	XMStoreFloat4x4(
		&m_constantBufferData.projection,
		XMMatrixTranspose(perspectiveMatrix)
		);

	XMStoreFloat4x4(&m_constantBufferData.view, XMMatrixTranspose(m_camera->GetView()));
	XMStoreFloat4x4(&m_constantBufferData.model, XMMatrixTranspose(XMMatrixScaling(0.001f, 0.001f, 0.001f)));
}

// Called once per frame, rotates the cube and calculates the model and view matrices.
void Renderer::Update(DX::StepTimer const& timer)
{
	m_camera->Update(timer);
}

void Renderer::Render()
{
	if (!m_loadingComplete)
	{
		return;
	}

	ClearViews();

	RenderGBuffers();
	
	RenderLightModels();
	m_profiler->nextTime("Render Things");
	
	AssignClusters();
	
	SortClusters();
	
	CalcClusterNums();
	m_profiler->nextTime("Assign Clusters");
	
	CalcLightBoundingBox();

	CalcZCodes();
	
	SortLights();
	m_profiler->nextTime("Sort Lights");

	ConstructLightTree();
	m_profiler->nextTime("Construct Light Tree");
	
	TraverseTree();
	m_profiler->nextTime("Traverse Light Tree");

	SumClusterLightCounts();

	MakeLightList();
	m_profiler->nextTime("Make Light Lists");

	RenderFinal();
	m_profiler->nextTime("Render Final Image");
}

std::shared_ptr<std::vector<_Lights::AABBNode>> Renderer::checkTree()
{
	auto context = m_deviceResources->GetD3DDeviceContext();
	// Verify tree looks as it should.
	ID3D11Buffer *tempBuffer;
	CD3D11_BUFFER_DESC tempDesc;
	tempDesc.ByteWidth = sizeof(_Lights::AABBNode) * _Lights::treeSize;
	tempDesc.StructureByteStride = sizeof(_Lights::AABBNode);
	tempDesc.BindFlags = 0;
	tempDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	tempDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	tempDesc.Usage = D3D11_USAGE_STAGING;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&tempDesc,
		nullptr,
		&tempBuffer
		)
		);
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	ZeroMemory(&mappedResource, sizeof(D3D11_MAPPED_SUBRESOURCE));
	context->CopyResource(tempBuffer, m_treeBuffer);
	context->Map(tempBuffer, 0, D3D11_MAP_READ, 0, &mappedResource);
	auto vec = std::make_shared<std::vector<_Lights::AABBNode>>(_Lights::treeSize);
	memcpy(&(*vec)[0], mappedResource.pData, sizeof(_Lights::AABBNode) * _Lights::treeSize);
	context->Unmap(tempBuffer, 0);
	tempBuffer->Release();
	return vec;
}

std::shared_ptr<std::vector<Cluster>> Renderer::checkClusters()
{
	auto context = m_deviceResources->GetD3DDeviceContext();
	// Verify tree looks as it should.
	ID3D11Buffer *tempBuffer;
	CD3D11_BUFFER_DESC tempDesc;
	tempDesc.ByteWidth = sizeof(Cluster) * MAXCLUSTERS;
	tempDesc.StructureByteStride = sizeof(Cluster);
	tempDesc.BindFlags = 0;
	tempDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	tempDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	tempDesc.Usage = D3D11_USAGE_STAGING;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&tempDesc,
		nullptr,
		&tempBuffer
		)
		);
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	ZeroMemory(&mappedResource, sizeof(D3D11_MAPPED_SUBRESOURCE));
	context->CopyResource(tempBuffer, m_clusterBuffer);
	context->Map(tempBuffer, 0, D3D11_MAP_READ, 0, &mappedResource);
	auto vec = std::make_shared<std::vector<Cluster>>(MAXCLUSTERS);
	memcpy(&(*vec)[0], mappedResource.pData, sizeof(Cluster) * MAXCLUSTERS);
	context->Unmap(tempBuffer, 0);
	tempBuffer->Release();
	return vec;
}

std::shared_ptr<std::vector<UINT32>> Renderer::checkLightList()
{
	auto context = m_deviceResources->GetD3DDeviceContext();
	// Verify tree looks as it should.
	ID3D11Buffer *tempBuffer;
	CD3D11_BUFFER_DESC tempDesc;
	tempDesc.ByteWidth = sizeof(UINT32) * MAXPAIRS;// Upper estimate on how many pairs there might be.
	tempDesc.StructureByteStride = sizeof(UINT32);
	tempDesc.BindFlags = 0;
	tempDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	tempDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	tempDesc.Usage = D3D11_USAGE_STAGING;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&tempDesc,
		nullptr,
		&tempBuffer
		)
		);
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	ZeroMemory(&mappedResource, sizeof(D3D11_MAPPED_SUBRESOURCE));
	ID3D11Resource *tempRes;
	m_clusterLightListsView->GetResource(&tempRes);
	context->CopyResource(tempBuffer, tempRes);
	context->Map(tempBuffer, 0, D3D11_MAP_READ, 0, &mappedResource);
	auto vec = std::make_shared<std::vector<UINT32>>(MAXPAIRS);
	memcpy(&(*vec)[0], mappedResource.pData, sizeof(UINT32) * MAXPAIRS);
	context->Unmap(tempBuffer, 0);
	tempBuffer->Release();
	return vec;
}

void Renderer::ClearViews() {
	auto context = m_deviceResources->GetD3DDeviceContext();
	context->ClearRenderTargetView(m_deviceResources->GetBackBufferRenderTargetView(), DirectX::Colors::CornflowerBlue);
	context->ClearDepthStencilView(m_deviceResources->GetDepthStencilView(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

	float zeroFloat[4] = { 0 };
	for (ID3D11RenderTargetView *view : m_deviceResources->m_d3dGBufferTargetViews)
	{
		if (view != nullptr)
			context->ClearRenderTargetView(view, zeroFloat);
	}
	context->ClearRenderTargetView(m_lightModelRTV, zeroFloat);

	UINT zeroUINT[4] = { 0 };
	context->ClearUnorderedAccessViewUint(m_deviceResources->m_clusterListUAV, zeroUINT);
	context->ClearUnorderedAccessViewUint(m_deviceResources->m_clusterOffsetUAV, zeroUINT);
	context->ClearUnorderedAccessViewUint(m_clusterView, zeroUINT);
	context->ClearUnorderedAccessViewUint(m_pairListView, zeroUINT);
	context->ClearUnorderedAccessViewUint(m_clusterLightListsView, zeroUINT);
}

void Renderer::RenderFinal()
{
	// Now we draw the full-screen quad.
	auto context = m_deviceResources->GetD3DDeviceContext();
	context->ClearDepthStencilView(m_deviceResources->GetDepthStencilView(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

	// Reset render targets to the screen.
	ID3D11RenderTargetView *const targets[1] = { m_deviceResources->GetBackBufferRenderTargetView() };
	context->OMSetRenderTargets(1, targets, m_deviceResources->GetDepthStencilView());

	// Attach our vertex shader.
	context->VSSetShader(
		m_finalVS.Get(),
		nullptr,
		0
		);
	// Attach our pixel shader.
	context->PSSetShader(
		m_finalPS.Get(),
		nullptr,
		0
		);

	m_lightScreenDimCBufferData.position = m_light;
	context->UpdateSubresource(
		m_lightScreenDimCBuffer.Get(),
		0,
		NULL,
		&m_lightScreenDimCBufferData,
		0,
		0
		);
	context->PSSetConstantBuffers(
		0,
		1,
		m_lightScreenDimCBuffer.GetAddressOf()
		);

	context->PSSetSamplers(
		0,
		1,
		m_pointSampler.GetAddressOf()
		);

	// Set the texture G-bufferinos for the final shading.
	context->PSSetShaderResources(0, m_deviceResources->GBUFFNUM, &m_deviceResources->m_d3dGBufferResourceViews[0]);
	context->PSSetShaderResources(m_deviceResources->GBUFFNUM, 1, &m_deviceResources->m_clusterListResourceView);
	// Set the other bufferinos for shading.
	context->PSSetShaderResources(m_deviceResources->GBUFFNUM + 1, 1, &m_clusterResourceView);
	context->PSSetShaderResources(m_deviceResources->GBUFFNUM + 2, 1, &m_clusterLightListsResourceView);
	context->PSSetShaderResources(m_deviceResources->GBUFFNUM + 3, 1, &m_lightListResourceView);
	context->PSSetShaderResources(m_deviceResources->GBUFFNUM + 4, 1, &m_lightModelSRV);
	
	// Set index and vertex buffers to NULL.
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, (DXGI_FORMAT)0, 0);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->Draw(3, 0);

	// Unset everything we used that's not going to be unset by the next draw.
	std::vector<ID3D11ShaderResourceView *> nullBuffer(m_deviceResources->GBUFFNUM + 5, 0);
	ID3D11Buffer *nullCBuffer[1] = { 0 };
	context->PSSetShaderResources(0, nullBuffer.size(), &nullBuffer[0]);
	context->PSSetConstantBuffers(0, 1, nullCBuffer);
}

void Renderer::AssignClusters()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	RECT windowSize = m_deviceResources->GetWindowSize();
	UINT width = windowSize.right - windowSize.left;
	UINT height = windowSize.bottom - windowSize.top;

	context->CSSetShader(
		m_assignClustersCS.Get(),
		nullptr,
		0);

	context->CSSetShaderResources(0, 1, &m_deviceResources->m_d3dGBufferResourceViews[1]);
	context->CSSetUnorderedAccessViews(0, 1, &m_deviceResources->m_clusterListUAV, 0);
	context->UpdateSubresource(m_assignClustersCBuffer.Get(), 0, NULL, &m_assignClustersCBufferData, 0, 0);
	context->CSSetConstantBuffers(0, 1, m_assignClustersCBuffer.GetAddressOf());

	context->Dispatch(width / 32, height / 32, 1);

	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	ID3D11ShaderResourceView *nullRV[1] = { 0 };
	ID3D11Buffer *nullCBuffer[1] = { 0 };
	context->CSSetShaderResources(0, 1, nullRV);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
	context->CSSetConstantBuffers(0, 1, nullCBuffer);
}

void Renderer::SortClusters()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	RECT windowSize = m_deviceResources->GetWindowSize();
	UINT width = windowSize.right - windowSize.left;
	UINT height = windowSize.bottom - windowSize.top;

	context->CSSetShader(
		m_sortClustersCS.Get(),
		nullptr,
		0);

	context->CSSetUnorderedAccessViews(0, 1, &m_deviceResources->m_clusterListUAV, 0);

	context->Dispatch(width / 32, height / 32, 1);

	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
}

void Renderer::CalcClusterNums()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	RECT windowSize = m_deviceResources->GetWindowSize();
	UINT width = windowSize.right - windowSize.left;
	UINT height = windowSize.bottom - windowSize.top;

	context->CSSetShader(
		m_compactClustersCS.Get(),
		nullptr,
		0);

	context->CSSetUnorderedAccessViews(0, 1, &m_deviceResources->m_clusterListUAV, 0);
	context->CSSetUnorderedAccessViews(1, 1, &m_deviceResources->m_clusterOffsetUAV, 0);
	context->CSSetUnorderedAccessViews(2, 1, &m_clusterView, 0);

	context->Dispatch(width / 32, height / 32, 1);
	
	context->CSSetShader(
		m_compactClusters2CS.Get(),
		nullptr,
		0);

	context->Dispatch(1, 1, 1);

	context->CSSetShader(
		m_finalClusterNumsCS.Get(),
		nullptr,
		0);

	context->Dispatch(width / 32, height / 32, 1);
	
	ID3D11UnorderedAccessView *nullUAV[3] = { 0 };
	context->CSSetUnorderedAccessViews(0, 3, nullUAV, 0);
}

void Renderer::CalcLightBoundingBox()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	//_Lights::lightsPerFrame();
	m_profiler->nextTime("Lights Per Frame (CPU mainly)");

	context->CSSetShader(
		m_prefixMinMaxLightPosCS.Get(),
		nullptr,
		0);

	// Update the light list resource.
	context->UpdateSubresource(m_lightListBuffer, 0, NULL, &_Lights::lights[0], 0, 0);

	/*D3D11_BOX region;
	region.left = (((1 << 5 * (_Lights::treeDepth - 1)) - 1) / 31) * sizeof(_Lights::AABBNode);
	region.right = region.left + _Lights::shaderLights.size() * sizeof(_Lights::AABBNode);
	region.front = region.top = 0;
	region.back = region.bottom = 1;

	context->UpdateSubresource(m_treeBuffer, 0, &region, &_Lights::shaderLights[0], 0, 0);*/
	context->CSSetUnorderedAccessViews(0, 1, &m_lightTreeView, 0);
	context->CSSetShaderResources(0, 1, &m_lightListResourceView);
	context->CSSetConstantBuffers(0, 1, &m_lightTreeCBuffer);
	context->CSSetConstantBuffers(1, 1, m_constantBuffer.GetAddressOf());
	
	UINT32 first = true;
	for (int i = _Lights::treeDepth - 1; i > 0; i -= 2) {
		int n = min(1 << 5 * i, _Lights::lights.size());
		_Lights::ConstantBuffer buff = { i, first };
		context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);
		context->Dispatch((n + 1023) / 1024, 1, 1);
		first = false;
	}

	ID3D11Buffer *nullCBuffer[2] = { 0 };
	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	context->CSSetConstantBuffers(0, 2, nullCBuffer);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
}

void Renderer::CalcZCodes()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	context->CSSetShader(
		m_calcZCodesCS.Get(),
		nullptr,
		0);

	context->CSSetUnorderedAccessViews(0, 1, &m_lightTreeView, 0);
	context->CSSetConstantBuffers(0, 1, &m_lightTreeCBuffer);

	int n = _Lights::lights.size();
	_Lights::ConstantBuffer buff = { _Lights::treeDepth - 1 };
	context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);
	context->Dispatch((n + 1023) / 1024, 1, 1);
	
	ID3D11Buffer *nullCBuffer[1] = { 0 };
	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	context->CSSetConstantBuffers(0, 1, nullCBuffer);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
}

void Renderer::SortLights()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	ID3D11Buffer* cbs[] = { m_lightTreeCBuffer, m_sortParameters.Get() };
	context->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);

	UINT32 lightCount = _Lights::lights.size();
	UINT32 treeOffset = ((1 << 5 * (_Lights::treeDepth - 1)) - 1) / 31;
	_Lights::ConstantBuffer buff = { lightCount, treeOffset };
	context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);

	context->CSSetUnorderedAccessViews(0, 1, &m_lightTreeView, nullptr);

	bool bDone = SortLightsInitial(lightCount);
	
	int presorted = 512;
	while (!bDone)
	{
		bDone = SortLightsIncremental(presorted, lightCount, treeOffset);
		presorted *= 2;
	}
	
	ID3D11Buffer *nullCBuffer[2] = { 0 };
	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	context->CSSetConstantBuffers(0, ARRAYSIZE(nullCBuffer), nullCBuffer);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
}

bool Renderer::SortLightsInitial(UINT32 maxSize)
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	bool bDone = true;

	// calculate how many threads we'll require:
	//   we'll sort 512 elements per CU (threadgroupsize 256)
	//     maybe need to optimize this or make it changeable during init
	//     TGS=256 is a good intermediate value


	unsigned int numThreadGroups = ((maxSize - 1) >> 9) + 1;

	assert(numThreadGroups <= 1024);


	if (numThreadGroups>1) bDone = false;

	// sort all buffers of size 512 (and presort bigger ones)
	context->CSSetShader(m_sort512CS.Get(), nullptr, 0);
	context->Dispatch(numThreadGroups, 1, 1);

	return bDone;
}

bool Renderer::SortLightsIncremental(UINT32 presorted, UINT32 maxSize, UINT32 treeOffset)
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	bool bDone = true;
	context->CSSetShader(m_sortStepCS.Get(), nullptr, 0);

	// prepare thread group description data
	unsigned int numThreadGroups = 0;

	if (maxSize > presorted)
	{
		if (maxSize>presorted * 2)
			bDone = false;

		unsigned int pow2 = presorted;
		while (pow2<maxSize)
			pow2 *= 2;
		numThreadGroups = pow2 >> 9;
	}

	unsigned int nMergeSize = presorted * 2;
	for (unsigned int nMergeSubSize = nMergeSize >> 1; nMergeSubSize>256; nMergeSubSize = nMergeSubSize >> 1)
	{
		D3D11_MAPPED_SUBRESOURCE MappedResource;

		context->Map(m_sortParameters.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedResource);
		XMINT4* sc = (XMINT4*)MappedResource.pData;
		sc->x = nMergeSubSize;
		if (nMergeSubSize == nMergeSize >> 1)
		{
			sc->y = (2 * nMergeSubSize - 1);
			sc->z = -1;
		}
		else
		{
			sc->y = nMergeSubSize;
			sc->z = 1;
		}
		sc->w = 0;
		context->Unmap(m_sortParameters.Get(), 0);

		context->Dispatch(numThreadGroups, 1, 1);
	}

	context->CSSetShader(m_sortInner512CS.Get(), nullptr, 0);
	context->Dispatch(numThreadGroups, 1, 1);

	return bDone;
}

void Renderer::ConstructLightTree()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	context->CSSetShader(
		m_AABBConstructTreeCS.Get(),
		nullptr,
		0);

	context->CSSetUnorderedAccessViews(0, 1, &m_lightTreeView, 0);
	context->CSSetConstantBuffers(0, 1, &m_lightTreeCBuffer);

#define BLOCKSIZE 4
	UINT32 first = true;
	for (int i = _Lights::treeDepth - 2; i > 0; i--) {
		int n = 1 << 5 * i;
		n = (n + BLOCKSIZE - 1) / BLOCKSIZE;
		_Lights::ConstantBuffer buff = { i, BLOCKSIZE, first };
		context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);
		context->Dispatch((n + 31) / 32, 1, 1);
		first = false;
	}

	ID3D11Buffer *nullCBuffer[1] = { 0 };
	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	context->CSSetConstantBuffers(0, 1, nullCBuffer);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
}

void Renderer::TraverseTree()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	// Get the number of clusters.
	/*D3D11_MAPPED_SUBRESOURCE mappedResource;
	ZeroMemory(&mappedResource, sizeof(D3D11_MAPPED_SUBRESOURCE));
	ID3D11Resource *temp;
	m_deviceResources->m_clusterOffsetUAV->GetResource(&temp);
	D3D11_BOX region;
	region.left = 31;
	region.right = 32;
	region.top = 23;
	region.bottom = 24;
	region.front = 0;
	region.back = 1;
	context->CopySubresourceRegion(m_tempCopyResource, 0, 0, 0, 0, temp, 0, &region);
	context->Map(m_tempCopyResource, 0, D3D11_MAP_READ, 0, &mappedResource);
	m_numClusters = *((UINT32 *)mappedResource.pData);
	context->Unmap(m_tempCopyResource, 0);*/

	// Traverse the tree for each cluster.
	context->CSSetShader(
		m_traverseTreeCS.Get(),
		nullptr,
		0);

	RECT windowSize = m_deviceResources->GetWindowSize();
	UINT width = windowSize.right - windowSize.left;
	UINT height = windowSize.bottom - windowSize.top;

	float denominator = log(1 + 2 * tan(m_fovAngleY * 0.5) / ((float)height / 32.0f));
	float invFocalLenX = tan(m_fovAngleY * 0.5) * (float)width / (float)height;
	float invFocalLenY = tan(m_fovAngleY * 0.5);
	_Lights::ConstantBuffer buff = { _Lights::treeDepth, width, height,
		*(UINT32*)&denominator, *(UINT32*)&invFocalLenX, *(UINT32*)&invFocalLenY };

	context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);

	context->CSSetConstantBuffers(0, 1, &m_lightTreeCBuffer);
	context->CSSetShaderResources(0, 1, &m_lightTreeResourceView);
	context->CSSetUnorderedAccessViews(0, 1, &m_clusterView, 0);
	UINT32 initialCount[1] = { 0 };
	context->CSSetUnorderedAccessViews(1, 1, &m_pairListView, initialCount);

	context->Dispatch((MAXCLUSTERS + 31) / 32, 1, 1);
	
	ID3D11Buffer *nullCBuffer[1] = { 0 };
	ID3D11ShaderResourceView *nullSRV[1] = { 0 };
	ID3D11UnorderedAccessView *nullUAV[2] = { 0 };
	context->CSSetConstantBuffers(0, 1, nullCBuffer);
	context->CSSetShaderResources(0, 1, nullSRV);
	context->CSSetUnorderedAccessViews(0, 2, nullUAV, 0);
}

void Renderer::SumClusterLightCounts()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	context->CSSetShader(
		m_sumClusterLightCountsCS.Get(),
		nullptr,
		0);

	context->CSSetConstantBuffers(0, 1, &m_lightTreeCBuffer);
	context->CSSetUnorderedAccessViews(0, 1, &m_clusterView, 0);

	_Lights::ConstantBuffer buff;
	UINT32 numClusters = MAXCLUSTERS;
	UINT32 stride = 1;
	std::vector<UINT32> strides;
	std::vector<UINT32> numClustersVec;

	while (numClusters > 1)
	{
		buff.val[0] = stride;
		buff.val[1] = (UINT32)false;
		numClusters = (numClusters + 1023) / 1024;
		context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);
		context->Dispatch(numClusters, 1, 1);
		numClustersVec.push_back(numClusters);
		strides.push_back(stride);
		stride *= 1024;
	}
	
	for (int i = strides.size() - 1; i >= 0; i--)
	{
		buff.val[0] = strides[i];
		buff.val[1] = (UINT32)true;
		context->UpdateSubresource(m_lightTreeCBuffer, 0, nullptr, &buff, 0, 0);
		context->Dispatch(numClustersVec[i], 1, 1);
	}

	ID3D11Buffer *nullCBuffer[1] = { 0 };
	ID3D11UnorderedAccessView *nullUAV[1] = { 0 };
	context->CSSetConstantBuffers(0, 1, nullCBuffer);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
}

void Renderer::MakeLightList()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	context->CSSetShader(
		m_makeLightListCS.Get(),
		nullptr,
		0);

	context->CSSetUnorderedAccessViews(0, 1, &m_clusterView, 0);
	context->CSSetUnorderedAccessViews(1, 1, &m_clusterLightListsView, 0);
	context->CSSetShaderResources(0, 1, &m_pairListResourceView);
	
	context->Dispatch((MAXPAIRS + 1023) / 1024, 1, 1);

	ID3D11UnorderedAccessView *nullUAV[2] = { 0 };
	ID3D11ShaderResourceView *nullSRV[1] = { 0 };
	context->CSSetUnorderedAccessViews(0, 2, nullUAV, 0);
	context->CSSetShaderResources(0, 1, nullSRV);
}

void Renderer::RenderGBuffers()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	// Set render targets.
	context->OMSetRenderTargets(
		m_deviceResources->GBUFFNUM,
		&m_deviceResources->m_d3dGBufferTargetViews[0],
		m_deviceResources->GetDepthStencilView()
		);

	// Attach our vertex shader.
	context->VSSetShader(
		m_deferredVS.Get(),
		nullptr,
		0
		);
	// Attach our pixel shader.
	context->PSSetShader(
		m_deferredPS.Get(),
		nullptr,
		0
		);

	// Send the constant buffer to the graphics device.
	XMStoreFloat4x4(&m_constantBufferData.view, XMMatrixTranspose(m_camera->GetView()));
	context->UpdateSubresource(
		m_constantBuffer.Get(),
		0,
		NULL,
		&m_constantBufferData,
		0,
		0
		);
	context->VSSetConstantBuffers(
		0,
		1,
		m_constantBuffer.GetAddressOf()
		);

	context->PSSetSamplers(
		0,
		1,
		m_linearSampler.GetAddressOf()
		);

	// Each vertex is one instance of the VertexPositionTextureNormal struct.
	context->IASetInputLayout(m_inputLayout.Get());
	UINT stride = sizeof(VertexPositionTextureNormal);
	UINT offset = 0;
	context->IASetVertexBuffers(
		0,
		1,
		m_vertexBuffer.GetAddressOf(),
		&stride,
		&offset
		);
	context->IASetIndexBuffer(
		m_indexBuffer.Get(),
		DXGI_FORMAT_R32_UINT, // Each index is one 32-bit unsigned integer.
		0
		);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (auto pMaterial : m_materials)
	{
		context->PSSetShaderResources(
			0,
			1,
			pMaterial->m_diffuseView.GetAddressOf()
			);

		for (int i : pMaterial->m_meshIndices)
		{
			context->DrawIndexed(
				m_meshes[i].m_nTriIndices * 3,
				m_meshes[i].m_triIndexOffset * 3,
				0
				);
		}
	}

	// Unset everything we used that's not going to be unset by the next draw.
	std::vector<ID3D11RenderTargetView *> nullRenderTargets(m_deviceResources->GBUFFNUM, 0);
	ID3D11DepthStencilView *nullDepthView = 0;
	context->OMSetRenderTargets(
		m_deviceResources->GBUFFNUM,
		&nullRenderTargets[0],
		nullDepthView
		);

	ID3D11Buffer *nullBuffer[1] = { 0 };
	context->VSSetConstantBuffers(
		0,
		1,
		nullBuffer
		);

	ID3D11ShaderResourceView *nullRV[1] = { 0 };
	context->PSSetShaderResources(0, 1, nullRV);
}

void Renderer::RenderLightModels()
{
	auto context = m_deviceResources->GetD3DDeviceContext();

	context->OMSetRenderTargets(1, &m_lightModelRTV, m_deviceResources->GetDepthStencilView());
	context->VSSetShader( m_lightModelVS, nullptr, 0 );
	context->PSSetShader( m_lightModelPS, nullptr, 0 );
	context->VSSetConstantBuffers( 0, 1, m_constantBuffer.GetAddressOf() );
	context->IASetInputLayout(m_lightInputLayout);

	unsigned int strides[2] = { sizeof(VertexPositionNormal), sizeof(LightInstance) };
	unsigned int offsets[2] = { 0, 0 };
	ID3D11Buffer* bufferPointers[2] = { m_lightVertexBuffer, m_lightInstanceBuffer };
	context->IASetVertexBuffers(0, 2, bufferPointers, strides, offsets);

	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->DrawInstanced(m_lightModel.size(), _Lights::lights.size(), 0, 0);

	ID3D11RenderTargetView *nullRTV[1] = { 0 };
	ID3D11DepthStencilView *nullDepthView = 0;
	context->OMSetRenderTargets( 1, nullRTV, nullDepthView );

	ID3D11Buffer *nullBuffer[1] = { 0 };
	context->VSSetConstantBuffers( 0, 1, nullBuffer );
}

void Renderer::CreateClusterResources()
{
	// Create the buffer to store the clusters.
	CD3D11_BUFFER_DESC clusterBufferDesc;
	clusterBufferDesc.ByteWidth = sizeof(Cluster) * MAXCLUSTERS;// Upper estimate on how many clusters there might be. Absolute maximum would be number of pixels.
	clusterBufferDesc.StructureByteStride = sizeof(Cluster);
	clusterBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	clusterBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	clusterBufferDesc.CPUAccessFlags = 0;
	clusterBufferDesc.Usage = D3D11_USAGE_DEFAULT;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&clusterBufferDesc,
		nullptr,
		&m_clusterBuffer
		)
		);

	D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc;
	UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
	UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.Flags = 0;
	UAVDesc.Buffer.NumElements = MAXCLUSTERS;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateUnorderedAccessView(
		m_clusterBuffer,
		&UAVDesc,
		&m_clusterView
		)
		);

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
	SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.NumElements = MAXCLUSTERS;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateShaderResourceView(
		m_clusterBuffer,
		&SRVDesc,
		&m_clusterResourceView
		)
		);

	// Create the buffer to store the pair list.
	CD3D11_BUFFER_DESC pairListBufferDesc;
	pairListBufferDesc.ByteWidth = sizeof(Pair) * MAXPAIRS;// Upper estimate on how many pairs there might be.
	pairListBufferDesc.StructureByteStride = sizeof(Pair);
	pairListBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	pairListBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	pairListBufferDesc.CPUAccessFlags = 0;
	pairListBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	ID3D11Buffer *pairListBuffer;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&pairListBufferDesc,
		nullptr,
		&pairListBuffer
		)
		);

	UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
	UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
	UAVDesc.Buffer.NumElements = MAXPAIRS;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateUnorderedAccessView(
		pairListBuffer,
		&UAVDesc,
		&m_pairListView
		)
		);

	SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.NumElements = MAXPAIRS;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateShaderResourceView(
		pairListBuffer,
		&SRVDesc,
		&m_pairListResourceView
		)
		);

	// Create the buffer to store the cluster light lists.
	CD3D11_BUFFER_DESC lightListsBufferDesc;
	lightListsBufferDesc.ByteWidth = sizeof(UINT32) * MAXPAIRS;// Upper estimate on how many pairs there might be.
	lightListsBufferDesc.StructureByteStride = sizeof(UINT32);
	lightListsBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	lightListsBufferDesc.MiscFlags = 0;
	lightListsBufferDesc.CPUAccessFlags = 0;
	lightListsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	ID3D11Buffer *lightListsBuffer;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&lightListsBufferDesc,
		nullptr,
		&lightListsBuffer
		)
		);

	UAVDesc.Format = DXGI_FORMAT_R32_UINT;
	UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.Flags = 0;
	UAVDesc.Buffer.NumElements = MAXPAIRS;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateUnorderedAccessView(
		lightListsBuffer,
		&UAVDesc,
		&m_clusterLightListsView
		)
		);

	SRVDesc;
	SRVDesc.Format = DXGI_FORMAT_R32_UINT;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.NumElements = MAXPAIRS;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateShaderResourceView(
		lightListsBuffer,
		&SRVDesc,
		&m_clusterLightListsResourceView
		)
		);
}

void Renderer::CreateLightResources()
{
	// Create the buffer to store the Light Tree.
	CD3D11_BUFFER_DESC lightTreeBufferDesc;
	lightTreeBufferDesc.ByteWidth = sizeof(_Lights::AABBNode) * _Lights::treeSize;
	lightTreeBufferDesc.StructureByteStride = sizeof(_Lights::AABBNode);
	lightTreeBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	lightTreeBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	lightTreeBufferDesc.CPUAccessFlags = 0;
	lightTreeBufferDesc.Usage = D3D11_USAGE_DEFAULT;

	std::vector<_Lights::AABBNode> initData(_Lights::treeSize, _Lights::AABBNode{ { 0, 0, 0 }, { 0, 0, 0 }, 0, 0 });
	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem = &initData[0];
	data.SysMemPitch = 0;
	data.SysMemSlicePitch = 0;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&lightTreeBufferDesc,
		&data,
		&m_treeBuffer
		)
		);

	D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc;
	UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
	UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.Flags = 0;
	UAVDesc.Buffer.NumElements = _Lights::treeSize;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateUnorderedAccessView(
		m_treeBuffer,
		&UAVDesc,
		&m_lightTreeView
		)
		);

	D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
	SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.NumElements = _Lights::treeSize;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateShaderResourceView(
		m_treeBuffer,
		&SRVDesc,
		&m_lightTreeResourceView
		)
		);

	CD3D11_BUFFER_DESC treeCBufferDesc(sizeof(_Lights::ConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&treeCBufferDesc,
		nullptr,
		&m_lightTreeCBuffer
		)
		);

	D3D11_TEXTURE2D_DESC textureDesc;
	ZeroMemory(&textureDesc, sizeof(textureDesc));
	textureDesc.Width = 1;
	textureDesc.Height = 1;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_R32_UINT;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_STAGING;
	textureDesc.BindFlags = 0;
	textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	textureDesc.MiscFlags = 0;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateTexture2D(
			&textureDesc,
			NULL,
			&m_tempCopyResource
			)
		);

	CD3D11_BUFFER_DESC lightListBufferDesc;
	lightListBufferDesc.ByteWidth = sizeof(_Lights::Light) * _Lights::lights.size();
	lightListBufferDesc.StructureByteStride = sizeof(_Lights::Light);
	lightListBufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	lightListBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	lightListBufferDesc.CPUAccessFlags = 0;
	lightListBufferDesc.Usage = D3D11_USAGE_DEFAULT;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&lightListBufferDesc,
		nullptr,
		&m_lightListBuffer
		)
		);

	SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.NumElements = _Lights::lights.size();
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateShaderResourceView(
		m_lightListBuffer,
		&SRVDesc,
		&m_lightListResourceView
		)
		);

	std::vector<byte> vsData = DX::ReadFile("LightModelVS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateVertexShader(
		&vsData[0],
		vsData.size(),
		nullptr,
		&m_lightModelVS
		)
		);

	std::vector<byte> psData = DX::ReadFile("LightModelPS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreatePixelShader(
		&psData[0],
		psData.size(),
		nullptr,
		&m_lightModelPS
		)
		);

	static const D3D11_INPUT_ELEMENT_DESC lightVertexDesc[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

		// Instance data.
		{ "INSTANCEPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		{ "INSTANCECOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
	};

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateInputLayout(
		lightVertexDesc,
		ARRAYSIZE(lightVertexDesc),
		&vsData[0],
		vsData.size(),
		&m_lightInputLayout
		)
		);
	/*gl.glVertex(100, 100, −100);
	gl.glVertex(100, 300, −100);
	gl.glVertex(300, 300, −100);

	gl.glVertex(300, 300, −300);
	gl.glVertex(300, 100, −300);

	gl.glVertex(100, 100, −100);
	gl.glVertex(100, 100, −300);

	gl.glVertex(100, 300, −300);
	gl.glVertex(100, 300, −100);

	gl.glVertex(100, 300, −100);
	gl.glVertex(100, 300, −300);

	gl.glVertex(100, 100, −300);
	gl.glVertex(300, 100, −300);*/
	m_lightModel = {
		{ { -1.0f, -1.0f, -1.0f, },{} }, // triangle 1 : begin
		{ {-1.0f, -1.0f, 1.0f, }, {} },
		{ { -1.0f, 1.0f, 1.0f, }, {} },// triangle 1 : end
		{ { 1.0f, 1.0f, -1.0f, }, {} },// triangle 2 : begin
		{ { -1.0f, -1.0f, -1.0f, }, {} },
		{ { -1.0f, 1.0f, -1.0f, }, {} },// triangle 2 : end
		{ { 1.0f, -1.0f, 1.0f, }, {} },
		{ { -1.0f, -1.0f, -1.0f, }, {} },
		{ { 1.0f, -1.0f, -1.0f, }, {} },
		{ { 1.0f, 1.0f, -1.0f, }, {} },
		{ { 1.0f, -1.0f, -1.0f, }, {} },
		{ { -1.0f, -1.0f, -1.0f, }, {} },
		{ { -1.0f, -1.0f, -1.0f, }, {} },
		{ { -1.0f, 1.0f, 1.0f, }, {} },
		{ { -1.0f, 1.0f, -1.0f, }, {} },
		{ { 1.0f, -1.0f, 1.0f, }, {} },
		{ { -1.0f, -1.0f, 1.0f, }, {} },
		{ { -1.0f, -1.0f, -1.0f, }, {} },
		{ { -1.0f, 1.0f, 1.0f, }, {} },
		{ { -1.0f, -1.0f, 1.0f, }, {} },
		{ { 1.0f, -1.0f, 1.0f, }, {} },
		{ { 1.0f, 1.0f, 1.0f, }, {} },
		{ { 1.0f, -1.0f, -1.0f, }, {} },
		{ { 1.0f, 1.0f, -1.0f, }, {} },
		{ { 1.0f, -1.0f, -1.0f, }, {} },
		{ { 1.0f, 1.0f, 1.0f, }, {} },
		{ { 1.0f, -1.0f, 1.0f, }, {} },
		{ { 1.0f, 1.0f, 1.0f, }, {} },
		{ { 1.0f, 1.0f, -1.0f, }, {} },
		{ { -1.0f, 1.0f, -1.0f, }, {} },
		{ { 1.0f, 1.0f, 1.0f, }, {} },
		{ { -1.0f, 1.0f, -1.0f, }, {} },
		{ { -1.0f, 1.0f, 1.0f, }, {} },
		{ { 1.0f, 1.0f, 1.0f, }, {} },
		{ { -1.0f, 1.0f, 1.0f, }, {} },
		{ { 1.0f, -1.0f, 1.0f }, {} },
	};
	D3D11_SUBRESOURCE_DATA lightVertexBufferData = { 0 };
	lightVertexBufferData.pSysMem = &m_lightModel[0];
	CD3D11_BUFFER_DESC lightVertexBufferDesc(sizeof(VertexPositionNormal) * m_lightModel.size(), D3D11_BIND_VERTEX_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&lightVertexBufferDesc,
		&lightVertexBufferData,
		&m_lightVertexBuffer
		)
		);

	std::vector<LightInstance> lightInstances(_Lights::lights.size());
	for (int i = 0; i < lightInstances.size(); i++)
	{
		lightInstances[i].pos = _Lights::lights[i].pos;
		lightInstances[i].color = _Lights::lights[i].color;
	}
	D3D11_SUBRESOURCE_DATA lightInstanceBufferData = { 0 };
	lightInstanceBufferData.pSysMem = &lightInstances[0];
	CD3D11_BUFFER_DESC lightInstanceBufferDesc(sizeof(LightInstance) * lightInstances.size(), D3D11_BIND_VERTEX_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&lightInstanceBufferDesc,
		&lightInstanceBufferData,
		&m_lightInstanceBuffer
		)
		);

	// Make render surface to draw lights on.
	RECT windowSize = m_deviceResources->GetWindowSize();
	UINT width = windowSize.right - windowSize.left;
	UINT height = windowSize.bottom - windowSize.top;

	ZeroMemory(&textureDesc, sizeof(textureDesc));
	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	textureDesc.CPUAccessFlags = 0;
	textureDesc.MiscFlags = 0;

	D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
	renderTargetViewDesc.Format = textureDesc.Format;
	renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	renderTargetViewDesc.Texture2D.MipSlice = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc;
	shaderResourceViewDesc.Format = textureDesc.Format;
	shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shaderResourceViewDesc.Texture2D.MostDetailedMip = 0;
	shaderResourceViewDesc.Texture2D.MipLevels = 1;

	ID3D11Texture2D *texture;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->
			CreateTexture2D(&textureDesc, NULL, &texture)
		);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->
			CreateRenderTargetView(texture, &renderTargetViewDesc, &m_lightModelRTV)
		);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->
			CreateShaderResourceView(texture, &shaderResourceViewDesc, &m_lightModelSRV)
		);
}

void Renderer::CreateDeviceDependentResources()
{
	std::vector<byte> vsData = DX::ReadFile("DeferredVS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateVertexShader(
		&vsData[0],
		vsData.size(),
		nullptr,
		&m_deferredVS
		)
		);

	static const D3D11_INPUT_ELEMENT_DESC vertexDesc[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateInputLayout(
		vertexDesc,
		ARRAYSIZE(vertexDesc),
		&vsData[0],
		vsData.size(),
		&m_inputLayout
		)
		);

	vsData = DX::ReadFile("FinalVS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateVertexShader(
			&vsData[0],
			vsData.size(),
			nullptr,
			&m_finalVS
			)
		);

	std::vector<byte> psData = DX::ReadFile("FinalPS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreatePixelShader(
			&psData[0],
			psData.size(),
			nullptr,
			&m_finalPS
			)
		);

	psData = DX::ReadFile("DeferredPS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreatePixelShader(
		&psData[0],
		psData.size(),
		nullptr,
		&m_deferredPS
		)
		);

	std::vector<byte> csData = DX::ReadFile("AssignClustersCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_assignClustersCS
		)
		);

	csData = DX::ReadFile("SortClustersCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_sortClustersCS
		)
		);
	
	csData = DX::ReadFile("CompactClustersCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_compactClustersCS
		)
		);

	csData = DX::ReadFile("CompactClusters2CS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_compactClusters2CS
		)
		);

	csData = DX::ReadFile("FinalClusterNumsCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_finalClusterNumsCS
		)
		);

	csData = DX::ReadFile("AABBConstructTreeCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_AABBConstructTreeCS
		)
		);

	csData = DX::ReadFile("PrefixMinMaxLightPosCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_prefixMinMaxLightPosCS
		)
		);

	csData = DX::ReadFile("CalcZCodesCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_calcZCodesCS
		)
		);

	csData = DX::ReadFile("Sort512CS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_sort512CS
		)
		);

	csData = DX::ReadFile("SortStepCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_sortStepCS
		)
		);

	csData = DX::ReadFile("SortInner512CS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_sortInner512CS
		)
		);

	csData = DX::ReadFile("TraverseTreeCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_traverseTreeCS
		)
		);
	
	csData = DX::ReadFile("SumClusterLightCountsCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_sumClusterLightCountsCS
		)
		);

	csData = DX::ReadFile("MakeLightListCS.cso");
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateComputeShader(
		&csData[0],
		csData.size(),
		nullptr,
		&m_makeLightListCS
		)
		);
	// Create MVP constant buffer.
	CD3D11_BUFFER_DESC constantBufferDesc(sizeof(ModelViewProjectionConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&constantBufferDesc,
		nullptr,
		&m_constantBuffer
		)
		);

	// Create Light constant buffer.
	CD3D11_BUFFER_DESC lightScreenDimCBufferDesc(sizeof(LightScreenDimBuffer), D3D11_BIND_CONSTANT_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&lightScreenDimCBufferDesc,
		nullptr,
		&m_lightScreenDimCBuffer
		)
		);

	// Create AssignClusters buffer.
	CD3D11_BUFFER_DESC assignClustersCBufferDesc(sizeof(AssignClustersBuffer), D3D11_BIND_CONSTANT_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
		&assignClustersCBufferDesc,
		nullptr,
		&m_assignClustersCBuffer
		)
		);

	D3D11_BUFFER_DESC cbDesc;
	ZeroMemory(&cbDesc, sizeof(cbDesc));
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbDesc.ByteWidth = sizeof(XMINT4);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(&cbDesc, nullptr, m_sortParameters.GetAddressOf())
		);

	LoadModels();

	// Once the model is loaded, create the buffers to store it.
	
	// Create vertex buffer.
	D3D11_SUBRESOURCE_DATA vertexBufferData = { 0 };
	vertexBufferData.pSysMem = &m_vertices[0];
	CD3D11_BUFFER_DESC vertexBufferDesc(sizeof(VertexPositionTextureNormal) * m_nVertices, D3D11_BIND_VERTEX_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
			&vertexBufferDesc,
			&vertexBufferData,
			&m_vertexBuffer
			)
		);

	// Create index buffer.
	D3D11_SUBRESOURCE_DATA indexBufferData = { 0 };
	indexBufferData.pSysMem = &m_triIndices[0];
	CD3D11_BUFFER_DESC indexBufferDesc(sizeof(TriangleIndices) * m_nTriIndices, D3D11_BIND_INDEX_BUFFER);
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateBuffer(
			&indexBufferDesc,
			&indexBufferData,
			&m_indexBuffer
			)
		);
	
	// Once the texture view is created, create a sampler.This defines how the color
	// for a particular texture coordinate is determined using the relevant texture data.
	D3D11_SAMPLER_DESC samplerDesc;
	ZeroMemory(&samplerDesc, sizeof(samplerDesc));

	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;

	// The sampler does not use anisotropic filtering, so this parameter is ignored.
	samplerDesc.MaxAnisotropy = 0;

	// Specify how texture coordinates outside of the range 0..1 are resolved.
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

	// Use no special MIP clamping or bias.
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	// Don't use a comparison function.
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateSamplerState(
			&samplerDesc,
			&m_linearSampler
			)
		);

	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateSamplerState(
		&samplerDesc,
		&m_pointSampler
		)
		);

	// Once the cube is loaded, the object is ready to be rendered.
	
	m_loadingComplete = true;
}

void Renderer::ReleaseDeviceDependentResources()
{
	m_loadingComplete = false;
	m_finalVS.Reset();
	m_inputLayout.Reset();
	m_finalPS.Reset();
	m_constantBuffer.Reset();
	m_vertexBuffer.Reset();
	m_indexBuffer.Reset();
	m_lightScreenDimCBuffer.Reset();
	m_materials.clear();
}