#include "gpu_scene.h"
#include "scene_progress.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <memory>

GPUScene::GeometryData GPUScene::packGeometry(const Model& model) {
	GeometryData result;
	const auto& faces = model.faces();
	const auto& materials = model.materials();
	std::unordered_map<const Material*, int> materialIds;
	materialIds.reserve(materials.size());
	for (int i = 0; i < int(materials.size()); ++i)
		materialIds[&materials[i]] = i;

	result.vertices.reserve(faces.size() * 3);
	result.normals.reserve(faces.size() * 3);
	result.uvs.reserve(faces.size() * 3);
	result.materialIds.reserve(faces.size());
	for (const auto& face : faces) {
		result.vertices.push_back(face.v[0]);
		result.vertices.push_back(face.v[1]);
		result.vertices.push_back(face.v[2]);

		result.normals.push_back(face.data[0] ? face.data[0]->normal : float3(0, 1, 0));
		result.normals.push_back(face.data[1] ? face.data[1]->normal : float3(0, 1, 0));
		result.normals.push_back(face.data[2] ? face.data[2]->normal : float3(0, 1, 0));

		result.uvs.push_back(face.data[0] ? face.data[0]->uv : float2(0));
		result.uvs.push_back(face.data[1] ? face.data[1]->uv : float2(0));
		result.uvs.push_back(face.data[2] ? face.data[2]->uv : float2(0));

		int materialId = 0;
		if (face.material) {
			auto it = materialIds.find(face.material);
			if (it != materialIds.end()) materialId = it->second;
		}
		result.materialIds.push_back(materialId);
	}
	return result;
}

	std::unique_ptr<GPUScene> GPUScene::load(const Model& model) {
	auto scene = std::make_unique<GPUScene>();
	// RENDER_EMISSION_SCALE moved OFF the load-time bake: a resident scene
	// (server registry) must render daytime (fixtures ~off) and evening
	// (fixtures on) views without a reload, so the scale is a per-render
	// GPURenderArgs knob applied in gpu_shadeBounce. Load stays raw (1.0).
	const float emissionScale = 1.0f;
	fprintf(stderr, "  load-stage: packGeometry\n");
	sceneProgress::report("geometry", 0, 1);
	GeometryData geometry = packGeometry(model);
	sceneProgress::report("geometry", 1, 1);
	fprintf(stderr, "  load-stage: collectTextures\n");
	TextureData textures = collectTextures(model);
	fprintf(stderr, "  load-stage: createMaterialData\n");
	sceneProgress::report("materials", 0, 1);
	MaterialData materials = createMaterialData(model, textures, emissionScale);
	fprintf(stderr, "  load-stage: applyMaterialTuning\n");
	applyMaterialTuning(model, materials);
	materials.shadowOpaque = classifyOpaqueMaterials(materials, textures);
	fprintf(stderr, "  load-stage: collectLights\n");
	LightData lights = collectLights(model, materials, emissionScale);
	sceneProgress::report("materials", 1, 1);
	fprintf(stderr, "  load-stage: uploadTextures\n");
	sceneProgress::report("upload", 0, 1);
	TextureUploadData textureUpload = uploadTextures(textures);
	SkyData sky = uploadSky(model);
	sceneProgress::report("upload", 1, 1);
	sceneProgress::report("sky", 1, 1);

	GPUMaterialBuffer gpuMaterials{};
	gpuMaterials.baseColorFactors = materials.baseColorFactors.data();
	gpuMaterials.roughnessFactors = materials.roughnessFactors.data();
	gpuMaterials.metallicFactors = materials.metallicFactors.data();
	gpuMaterials.specularFactors = materials.specularFactors.data();
	gpuMaterials.opacities = materials.opacities.data();
	gpuMaterials.iors = materials.iors.data();
	gpuMaterials.emissionFactors = materials.emissionFactors.data();
	gpuMaterials.transmittingColors = nullptr;
	gpuMaterials.baseColorTexIds = materials.baseColorTexIds.data();
	gpuMaterials.normalTexIds = materials.normalTexIds.data();
	gpuMaterials.roughnessTexIds = materials.roughnessTexIds.data();
	gpuMaterials.metallicTexIds = materials.metallicTexIds.data();
	gpuMaterials.emissionTexIds = materials.emissionTexIds.data();
	gpuMaterials.packedSpecFlags = materials.packedSpecFlags.data();
	gpuMaterials.emissionAxis = materials.emissionAxis.data();
	gpuMaterials.emissionLobeK = materials.emissionLobeK.data();
	gpuMaterials.emissionLobeU = materials.emissionLobeU.data();
	gpuMaterials.emissionGamma = materials.emissionGamma.data();
	gpuMaterials.alphaCutout = materials.alphaCutout.data();
	gpuMaterials.uvFlipU = materials.uvFlipU.data();
	gpuMaterials.shadowOpaque = materials.shadowOpaque.data();
	gpuMaterials.numMaterials = int(model.materials().size());
	gpuMaterials.texObjects = textureUpload.objects;
	gpuMaterials.numTextures = int(textures.unique.size());
	gpuMaterials.texData = textureUpload.rawData;
	gpuMaterials.texDataOffsets = textureUpload.texelOffsets.data();
	gpuMaterials.texDataWidths = textureUpload.widths;
	gpuMaterials.texDataHeights = textureUpload.heights;
	gpuMaterials.texFormats = textureUpload.formats.data();
	gpuMaterials.texByteOffsets = textureUpload.byteOffsets.data();
	gpuMaterials.mipData = textureUpload.mipRawData;
	gpuMaterials.mipByteOffsets = textureUpload.mipByteOffsetsGpu;
	gpuMaterials.mipLevels = textureUpload.mipLevelsGpu;
	gpuMaterials.mipWidths = textureUpload.mipWidthsGpu;
	gpuMaterials.mipHeights = textureUpload.mipHeightsGpu;
	gpuMaterials.mipLevelStart = textureUpload.mipStartGpu;

	LinearBVHNode* flatBvh = flattenBVH(model);
	fprintf(stderr, "  load-stage: uploadModel\n");
	GPUModelData gpuModel = uploadModel(
		geometry, flatBvh, model.bvh().numNodesTotal(), gpuMaterials,
		lights.triangles.data(), int(lights.triangles.size()),
		lights.trianglePrefix.data(), lights.objectFirst.data(),
		lights.objectArea.data(), lights.objectPower.data(),
		lights.objectCentroid.data(), int(lights.objectArea.size()));
	delete[] flatBvh;
	scene->attach(UploadedResources{
		gpuModel, textureUpload.objects, textureUpload.uploaded,
		textureUpload.rawData, textureUpload.widths, textureUpload.heights,
		textureUpload.mipRawData,
		sky.texture, sky.cdf, sky.cdfSum, sky.width, sky.height});
	return scene;
}

GPUScene::TextureData GPUScene::collectTextures(const Model& model) {
	TextureData result;
	const auto& materials = model.materials();
	result.unique.reserve(materials.size());
	result.ids.reserve(materials.size() * 5);

	auto registerTexture = [&](ImageTexture* texture) {
		if (!texture) return;
		if (result.ids.find(texture) != result.ids.end()) return;
		const int id = int(result.unique.size());
		result.unique.push_back(texture);
		result.ids[texture] = id;
		result.widths.push_back(texture->getWidth());
		result.heights.push_back(texture->getHeight());
		result.channels.push_back(texture->getChannels());
		result.data.push_back(texture->getData());
		if (id < 3 && texture->getData()) {
			const uint8_t* data = texture->getData();
			fprintf(stderr,
					"  TEX_DATA[%d]: first 8 bytes = %02x %02x %02x %02x %02x %02x %02x %02x\n",
					id, data[0], data[1], data[2], data[3],
					data[4], data[5], data[6], data[7]);
		}
	};

	for (const auto& material : materials) {
		registerTexture(material.baseColorMap);
		registerTexture(material.normalMap);
		registerTexture(material.roughnessMap);
		registerTexture(material.metallicMap);
		registerTexture(material.emissionMap);
	}
	// P0: mip chains must be built in each texture's DECODE domain:
	//   baseColor -> pow 2.2 (shader gpu_toLinear)
	//   emission  -> pow(emissionGamma): 5.0 default, 2.2 for string lights
	//                (the shader raises the sampled texel to emG directly)
	//   normal/roughness/metallic -> linear bytes (unchanged; not decoded)
	// Averaging in the wrong domain bias-darkens every mip (Jensen), which
	// measured as a 9% interior luma drop before this existed.
	{
		std::unordered_map<ImageTexture*, float> gammaUse;
		for (const auto& material : materials) {
			if (material.baseColorMap) gammaUse.emplace(material.baseColorMap, 2.2f);
			if (material.emissionMap) {
				std::string lower = material.name;
				for (auto& ch : lower)
					ch = char(std::tolower(static_cast<unsigned char>(ch)));
				const float g = lower.find("stringlight") != std::string::npos ? 2.2f : 5.0f;
				auto it = gammaUse.find(material.emissionMap);
				if (it == gammaUse.end() || it->second < g) gammaUse[material.emissionMap] = g;
			}
		}
		for (auto* tex : result.unique) {
			auto it = gammaUse.find(tex);
			if (tex && it != gammaUse.end()) tex->rebuildMipmapsSrgb(it->second);
		}
	}
	return result;
}

GPUScene::MaterialData GPUScene::createMaterialData(const Model& model,
													const TextureData& textures,
													float emissionScale) {
	MaterialData result;
	const auto& materials = model.materials();
	const int count = int(materials.size());
	result.baseColorFactors.resize(count);
	result.emissionFactors.resize(count);
	result.roughnessFactors.resize(count);
	result.metallicFactors.resize(count);
	result.specularFactors.resize(count);
	result.opacities.resize(count);
	result.iors.resize(count);
	result.baseColorTexIds.assign(count, -1);
	result.normalTexIds.assign(count, -1);
	result.roughnessTexIds.assign(count, -1);
	result.metallicTexIds.assign(count, -1);
	result.emissionTexIds.assign(count, -1);
	result.packedSpecFlags.assign(count, 0);
	result.alphaCutout.assign(count, 0);
	result.uvFlipU.assign(count, 0);
	result.shadowOpaque.assign(count, 0);
	result.emissionAxis.assign(count, float3(0.0f));
	result.emissionLobeK.assign(count, 0.0f);
	result.emissionLobeU.assign(count, 0.0f);
	result.emissionGamma.assign(count, 5.0f);

	auto textureId = [&textures](ImageTexture* texture) {
		if (!texture) return -1;
		auto it = textures.ids.find(texture);
		return it == textures.ids.end() ? -1 : it->second;
	};
	for (int i = 0; i < count; ++i) {
		const auto& material = materials[i];
		result.baseColorFactors[i] = material.baseColorFactor;
		result.emissionFactors[i] = material.emissionFactor * emissionScale;
		result.roughnessFactors[i] = material.roughnessFactor;
		result.metallicFactors[i] = material.metallicFactor;
		result.specularFactors[i] = material.specularFactor;
		result.opacities[i] = material.opacity;
		result.iors[i] = material.ior;
		result.baseColorTexIds[i] = textureId(material.baseColorMap);
		result.normalTexIds[i] = textureId(material.normalMap);
		result.roughnessTexIds[i] = textureId(material.roughnessMap);
		result.metallicTexIds[i] = textureId(material.metallicMap);
		result.emissionTexIds[i] = textureId(material.emissionMap);
		result.packedSpecFlags[i] = material.packedSpecularMap ? 1 : 0;
	}
	return result;
}

void GPUScene::applyMaterialTuning(const Model& model, MaterialData& materials) {
	const auto& modelMaterials = model.materials();
	const bool gloss = std::getenv("RENDER_GLOSS") && std::getenv("RENDER_GLOSS")[0] == '1';
	float facadeAlbedo = 1.0f;
	if (const char* value = std::getenv("RENDER_FACADE_ALBEDO"))
		facadeAlbedo = fmaxf(0.0f, std::strtof(value, nullptr));
	float ceilingEmission = 1.0f;
	if (const char* value = std::getenv("RENDER_CEILING_EMI"))
		ceilingEmission = std::strtof(value, nullptr);
	float emissionLobe = 2.0f;
	if (const char* value = std::getenv("RENDER_EMI_LOBE"))
		emissionLobe = std::strtof(value, nullptr);
	float uniformEmission = 0.3f;
	if (const char* value = std::getenv("RENDER_EMI_UNIFORM"))
		uniformEmission = glm::clamp(std::strtof(value, nullptr), 0.0f, 1.0f);
	float fixtureGain = 1.0f;
	if (const char* value = std::getenv("RENDER_FIXTURE_GAIN"))
		fixtureGain = std::strtof(value, nullptr);

	for (size_t i = 0; i < modelMaterials.size(); ++i) {
		const std::string& name = modelMaterials[i].name;
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(),
					   [](unsigned char c) { return std::tolower(c); });
		std::string normalized;
		for (char c : lower) if (c != '_' && c != ' ') normalized += c;

		materials.packedSpecFlags[i] = modelMaterials[i].packedSpecularMap
			? (gloss ? 2 : 1) : 0;
		if (facadeAlbedo < 1.0f &&
			(lower.find("plaster") != std::string::npos ||
			 lower.find("concrete") != std::string::npos ||
			 lower.find("balcony_concrete") != std::string::npos ||
			 lower.find("balcony_trims") != std::string::npos))
			materials.baseColorFactors[i] *= facadeAlbedo;

		if (normalized.find("stringlight") != std::string::npos)
			materials.emissionGamma[i] = 2.2f;
		const bool fixture = normalized.find("ceilinglamp") != std::string::npos ||
							 normalized.find("ceilingfan") != std::string::npos ||
							 normalized.find("walllight") != std::string::npos ||
							 normalized.find("lantern") != std::string::npos;
		if (fixture) {
			if (emissionLobe > 0.0f) {
				materials.emissionAxis[i] = float3(0.0f, -1.0f, 0.0f);
				materials.emissionLobeK[i] = emissionLobe;
				materials.emissionLobeU[i] = uniformEmission;
			}
			materials.emissionFactors[i] *= ceilingEmission * fixtureGain;
		}

		const char* flipList = std::getenv("RENDER_UVFLIP_U");
		if (flipList) {
			std::string list(flipList);
			size_t begin = 0;
			while (begin < list.size()) {
				size_t end = list.find(',', begin);
				std::string token = list.substr(begin, end == std::string::npos
														? std::string::npos : end - begin);
				std::transform(token.begin(), token.end(), token.begin(),
							   [](unsigned char c) { return std::tolower(c); });
				std::string normalizedToken;
				for (char c : token) if (c != '_' && c != ' ') normalizedToken += c;
				if (!normalizedToken.empty() && normalized.find(normalizedToken) != std::string::npos)
					materials.uvFlipU[i] = 1;
				if (end == std::string::npos) break;
				begin = end + 1;
			}
		}
		if (normalized.find("menusign") != std::string::npos)
			materials.uvFlipU[i] = 2;
		if (normalized.find("foliage") != std::string::npos ||
			normalized.find("flower") != std::string::npos ||
			normalized.find("hedge") != std::string::npos ||
			normalized.find("leaf") != std::string::npos ||
			normalized.find("plant") != std::string::npos ||
			normalized.find("bush") != std::string::npos ||
			normalized.find("grass") != std::string::npos)
			materials.alphaCutout[i] = 1;
	}
}

std::vector<int> GPUScene::classifyOpaqueMaterials(const MaterialData& materials,
												   const TextureData& textures) {
	std::vector<int> minimumAlpha(textures.unique.size(), 255);
	for (size_t textureIndex = 0; textureIndex < textures.unique.size(); ++textureIndex) {
		const unsigned char* data = textures.data[textureIndex];
		const int channels = textures.channels[textureIndex];
		const int width = textures.widths[textureIndex];
		const int height = textures.heights[textureIndex];
		if (!data || channels < 4 || width <= 0 || height <= 0) continue;
		int minimum = 255;
		const size_t texelCount = size_t(width) * height;
		for (size_t texel = 0; texel < texelCount; texel += 16)
			minimum = std::min(minimum, int(data[texel * channels + 3]));
		minimumAlpha[textureIndex] = minimum;
	}

	std::vector<int> result(materials.opacities.size(), 0);
	int opaqueCount = 0;
	for (size_t materialIndex = 0; materialIndex < result.size(); ++materialIndex) {
		if (materials.alphaCutout[materialIndex] ||
			materials.opacities[materialIndex] < 0.99f)
			continue;
		const int textureId = materials.baseColorTexIds[materialIndex];
		const float minimum = textureId >= 0 && textureId < int(minimumAlpha.size())
			? float(minimumAlpha[textureId]) * (1.0f / 255.0f) : 1.0f;
		if (materials.opacities[materialIndex] * minimum < 0.99f) continue;
		result[materialIndex] = 1;
		++opaqueCount;
	}
	fprintf(stderr, "  SHADOWFAST: %d/%zu materials are provably opaque blockers\n",
			opaqueCount, result.size());
	return result;
}

GPUScene::LightData GPUScene::collectLights(const Model& model,
											const MaterialData& materials,
											float emissionScale) {
	LightData result;
	const auto& faces = model.faces();
	const auto& modelMaterials = model.materials();
	std::unordered_map<const Material*, int> materialIds;
	for (int i = 0; i < int(modelMaterials.size()); ++i)
		materialIds[&modelMaterials[i]] = i;

	struct Record {
		GPULightTri triangle;
		const Material* material = nullptr;
	};
	std::vector<Record> records;
	for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
		const Face& face = faces[faceIndex];
		if (!face.material || !face.material->isEmissive()) continue;
		Record record;
		record.material = face.material;
		record.triangle.v0 = face.v[0];
		record.triangle.v1 = face.v[1];
		record.triangle.v2 = face.v[2];
		record.triangle.faceId = int(faceIndex);
		record.triangle.matId = materialIds[face.material];
		float3 emission = face.material->emissionFactor * emissionScale;
		const int materialId = record.triangle.matId;
		const float gamma = materialId >= 0 && materialId < int(materials.emissionLobeK.size())
			? materials.emissionGamma[materialId] : 5.0f;
		if (face.material->emissionMap && !face.material->emissionMap->empty()) {
			const ImageTexture& texture = *face.material->emissionMap;
			const float2 uv = (face.data[0]->uv + face.data[1]->uv + face.data[2]->uv) * (1.0f / 3.0f);
			const float4 texel = texture.sample(uv.x, uv.y, 0.0f);
			emission *= float3(std::pow(fmaxf(texel.x, 0.0f), gamma),
							   std::pow(fmaxf(texel.y, 0.0f), gamma),
							   std::pow(fmaxf(texel.z, 0.0f), gamma));
		}
		const float luminance = dot(emission, float3(0.299f, 0.587f, 0.114f));
		if (luminance > 200.0f) emission *= 200.0f / luminance;
		record.triangle.emission = emission;
		const float3 edge1 = face.v[1] - face.v[0];
		const float3 edge2 = face.v[2] - face.v[0];
		record.triangle.area = 0.5f * glm::length(glm::cross(edge1, edge2));
		records.push_back(record);
	}

	std::vector<const Material*> objects;
	std::unordered_map<const Material*, int> objectIds;
	for (const auto& record : records) {
		if (objectIds.find(record.material) == objectIds.end()) {
			objectIds[record.material] = int(objects.size());
			objects.push_back(record.material);
		}
	}
	std::sort(records.begin(), records.end(), [&](const Record& a, const Record& b) {
		return objectIds[a.material] < objectIds[b.material];
	});

	const int objectCount = int(objects.size());
	result.objectFirst.assign(objectCount + 1, 0);
	result.objectArea.assign(objectCount, 0.0f);
	result.objectPower.assign(objectCount, 0.0f);
	result.objectCentroid.assign(objectCount, float3(0.0f));
	std::vector<double> cx(objectCount, 0.0), cy(objectCount, 0.0), cz(objectCount, 0.0);
	result.triangles.reserve(records.size());
	for (const auto& record : records) {
		const int object = objectIds[record.material];
		result.objectFirst[object + 1]++;
		result.objectArea[object] += record.triangle.area;
		const float weight = record.triangle.area / 3.0f;
		cx[object] += double(record.triangle.v0.x + record.triangle.v1.x + record.triangle.v2.x) * weight;
		cy[object] += double(record.triangle.v0.y + record.triangle.v1.y + record.triangle.v2.y) * weight;
		cz[object] += double(record.triangle.v0.z + record.triangle.v1.z + record.triangle.v2.z) * weight;
		result.triangles.push_back(record.triangle);
	}
	result.trianglePrefix.resize(result.triangles.size());
	float areaPrefix = 0.0f;
	for (size_t i = 0; i < result.triangles.size(); ++i) {
		areaPrefix += result.triangles[i].area;
		result.trianglePrefix[i] = areaPrefix;
	}
	for (int object = 0; object < objectCount; ++object) {
		result.objectFirst[object + 1] += result.objectFirst[object];
		if (result.objectArea[object] > 0.0f)
			result.objectCentroid[object] = float3(
				float(cx[object] / result.objectArea[object]),
				float(cy[object] / result.objectArea[object]),
				float(cz[object] / result.objectArea[object]));
		if (result.objectFirst[object] < int(result.triangles.size())) {
			const auto& emission = result.triangles[result.objectFirst[object]].emission;
			const float luminance = dot(emission, float3(0.299f, 0.587f, 0.114f));
			result.objectPower[object] = fmaxf(1e-8f, luminance * result.objectArea[object]);
		}
	}
	fprintf(stderr, "LIGHTSTAT: %zu emissive tris in %d objects\n",
			result.triangles.size(), objectCount);
	return result;
}

GPUScene::TextureUploadData GPUScene::uploadTextures(const TextureData& textures) {
	TextureUploadData result;
	const size_t count = textures.unique.size();
	result.formats.assign(count, 0);
	result.blocks.assign(count, nullptr);
	result.byteOffsets.assign(count + 1, 0);
	result.texelOffsets.assign(count + 1, 0);

	long long byteCursor = 0;
	int bcCount = 0, rgbaCount = 0;
	int bcEnable = 1, bcMask = 7;
	if (const char* value = std::getenv("RENDER_BC_TEX"))
		bcEnable = std::atoi(value) != 0;
	if (const char* value = std::getenv("RENDER_BC_FMT"))
		bcMask = std::atoi(value);
	for (size_t i = 0; i < count; ++i) {
		result.byteOffsets[i] = byteCursor;
		const ImageTexture* texture = textures.unique[i];
		const int width = textures.widths[i];
		const int height = textures.heights[i];
		const int format = texture && !texture->compressedBlocks.empty() && !texture->cpuModified
			? texture->bcFormat : -1;
		if (bcEnable && format > 0 && (bcMask & (1 << (format - 1))) &&
			width > 0 && height > 0) {
			const size_t blockWidth = (size_t(width) + 3) / 4;
			const size_t blockHeight = (size_t(height) + 3) / 4;
			const size_t bytes = blockWidth * blockHeight * (format == 1 ? 8u : 16u);
			if (bytes <= texture->compressedBlocks.size()) {
				result.formats[i] = format;
				result.blocks[i] = texture->compressedBlocks.data();
				byteCursor += static_cast<long long>(bytes);
				++bcCount;
				continue;
			}
		}
		byteCursor += static_cast<long long>(width) * height * 4;
		++rgbaCount;
	}
	result.byteOffsets[count] = byteCursor;
	int texelCursor = 0;
	for (size_t i = 0; i < count; ++i) {
		result.texelOffsets[i] = texelCursor;
		texelCursor += textures.widths[i] * textures.heights[i];
	}
	result.texelOffsets[count] = texelCursor;

	// ---- P0 mipmap chain staging ----
	// Levels 1..N (RGBA8, built by generateMipmaps at load) are uploaded to a
	// SEPARATE GPU buffer so the level-0 layout (byteOffsets/texelOffsets)
	// stays bit-identical. Textures with channels != 4 (e.g. HDR-loaded 3ch)
	// are skipped: the kernel's RGBA8 mip fetch assumes 4 bytes/texel.
	// Levels are separate allocations (not contiguous) → flat per-level
	// pointer/size arrays + per-texture [start,count] + per-texture GPU byte
	// offsets are all the kernel needs.
	result.mipLevelCounts.assign(count, 0);
	result.mipLevelStart.assign(count + 1, 0);
	std::vector<long long> mipTexByteOffsets(count + 1, 0);
	long long mipCursor = 0;
	int levelIdx = 0;
	for (size_t i = 0; i < count; ++i) {
		result.mipLevelStart[i] = levelIdx;
		mipTexByteOffsets[i] = mipCursor;
		const ImageTexture* tex = textures.unique[i];
		if (!tex || tex->getChannels() != 4) continue;
		const int levels = tex->getMipLevels();
		int stored = 0;
		for (int l = 1; l <= levels; ++l) {
			const int mw = tex->getMipWidth(l), mh = tex->getMipHeight(l);
			if (mw <= 0 || mh <= 0 || !tex->getMipData(l)) continue;
			result.mipLevelPointers.push_back(tex->getMipData(l));
			result.mipLevelWidths.push_back(mw);
			result.mipLevelHeights.push_back(mh);
			mipCursor += (long long)mw * mh * 4;
			++levelIdx; ++stored;
		}
		result.mipLevelCounts[i] = stored;
	}
	result.mipLevelStart[count] = levelIdx;
	mipTexByteOffsets[count] = mipCursor;
	result.mipTexByteOffsets = mipTexByteOffsets;
	fprintf(stderr, "  Texture storage: %d BC-compressed + %d RGBA8 (%.2f GB GPU layout), "
			"%d mip levels staged (%.2f GB)\n",
			bcCount, rgbaCount, double(byteCursor) / 1e9, levelIdx,
			double(mipCursor) / 1e9);

	if (count > 0) {
		result.uploaded = uploadTexturesToGPU(
			textures.widths.data(), textures.heights.data(), textures.channels.data(),
			textures.data.data(), result.formats.data(), result.blocks.data(),
			result.byteOffsets.data(), int(count), &result.objects, &result.widths,
			&result.heights, &result.rawData,
			result.mipLevelCounts.empty() ? nullptr : result.mipLevelCounts.data(),
			result.mipLevelStart.empty() ? nullptr : result.mipLevelStart.data(),
			result.mipTexByteOffsets.empty() ? nullptr : result.mipTexByteOffsets.data(),
			result.mipLevelPointers.empty() ? nullptr : result.mipLevelPointers.data(),
			result.mipLevelWidths.empty() ? nullptr : result.mipLevelWidths.data(),
			result.mipLevelHeights.empty() ? nullptr : result.mipLevelHeights.data(),
			&result.mipRawData, &result.mipByteOffsetsGpu, &result.mipLevelsGpu,
			&result.mipWidthsGpu, &result.mipHeightsGpu, &result.mipStartGpu);
		if (result.uploaded < int(count))
			fprintf(stderr, "!! WARNING: only %d/%zu textures uploaded to GPU\n",
					result.uploaded, count);
	}
	return result;
}

LinearBVHNode* GPUScene::flattenBVH(const Model& model) {
	return model.bvh().flattenForGPU();
}

GPUScene::SkyData GPUScene::uploadSky(const Model& model) {
	SkyData result;
	const auto& sky = model.skyMap();
	if (sky.empty() || sky.getWidth() <= 0 || sky.getHeight() <= 0 || !sky.getData())
		return result;

	const int pixelCount = sky.getWidth() * sky.getHeight();
	std::vector<float> flatData(size_t(pixelCount) * 3);
	const float3* source = sky.getData();
	for (int i = 0; i < pixelCount; ++i) {
		flatData[size_t(i) * 3 + 0] = source[i].x;
		flatData[size_t(i) * 3 + 1] = source[i].y;
		flatData[size_t(i) * 3 + 2] = source[i].z;
	}

	result.texture = uploadSkyBoxToGPU_float(flatData.data(), sky.getWidth(), sky.getHeight());
	result.width = sky.getWidth();
	result.height = sky.getHeight();
	const auto& cdf = sky.getCDF();
	if (!cdf.empty()) {
		result.cdf = uploadSkyCDF(cdf.data(), int(cdf.size()));
		result.cdfSum = sky.getCDFSum();
	}
	return result;
}

GPUModelData GPUScene::uploadModel(const GeometryData& geometry,
								   const LinearBVHNode* bvhNodes,
								   int numBVHNodes,
								   const GPUMaterialBuffer& materials,
								   const GPULightTri* lightTriangles,
								   int numLightTriangles,
								   const float* trianglePrefix,
								   const int* objectFirst,
								   const float* objectArea,
								   const float* objectPower,
								   const float3* objectCentroid,
								   int numObjects) {
	return uploadModelToGPU(
		geometry.vertices.data(), int(geometry.vertices.size()),
		geometry.normals.data(), geometry.uvs.data(),
		geometry.materialIds.data(), int(geometry.materialIds.size()),
		bvhNodes, numBVHNodes, materials,
		lightTriangles, numLightTriangles, trianglePrefix,
		objectFirst, objectArea, objectPower, objectCentroid, numObjects);
}