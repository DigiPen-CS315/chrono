#include "ChCCollisionGPU.h"
#include "ChCCollisionGPU.cuh"

using namespace chrono::collision;
// bounding box type
typedef thrust::pair<float3, float3> bbox;

// reduce a pair of bounding boxes (a,b) to a bounding box containing a and b
struct bbox_reduction: public thrust::binary_function<bbox, bbox, bbox> {
		__host__ __device__
		bbox operator()(bbox a, bbox b) {
			float3 ll = F3(fminf(a.first.x, b.first.x), fminf(a.first.y, b.first.y), fminf(a.first.z, b.first.z));// lower left corner
			float3 ur = F3(fmaxf(a.second.x, b.second.x), fmaxf(a.second.y, b.second.y), fmaxf(a.second.z, b.second.z));// upper right corner
			return bbox(ll, ur);
		}
};

// convert a point to a bbox containing that point, (point) -> (point, point)
struct bbox_transformation: public thrust::unary_function<float3, bbox> {
		__host__ __device__
		bbox operator()(float3 point) {
			return bbox(point, point);
		}
};

__global__ void Compute_AABBs(float3* pos, float4* rot, float3* obA, float3* obB, float3* obC, float4* obR, int3* typ, float3* aabb) {
	uint index = blockIdx.x * blockDim.x + threadIdx.x;
	if (index >= number_of_models_const) {
		return;
	}
	int3 type = typ[index];

	float3 A = obA[index];
	float3 B = obB[index];
	float3 C = obC[index];

	float3 position = pos[type.z];
	float4 rotation = normalize(rot[type.z] + obR[index]);

	float3 temp_min;
	float3 temp_max;

	if (type.x == 0) {
		ComputeAABBSphere(B.x, A + position, temp_min, temp_max);

	} else if (type.x == 5) {
		A = quatRotate(A + position, rotation);
		B = quatRotate(B + position, rotation);
		C = quatRotate(C + position, rotation);

		ComputeAABBTriangle(A, B, C, temp_min, temp_max);

	} else if (type.x == 1 || type.x == 2 || type.x == 3) {

		ComputeAABBBox(B, A + position, rotation, temp_min, temp_max);

	} else {
		return;
	}
	aabb[index] = temp_min;
	aabb[index + number_of_models_const] = temp_max;

}

__global__ void Offset_AABBs(float3* points) {
	uint index = blockIdx.x * blockDim.x + threadIdx.x;
	if (index >= number_of_models_const) {
		return;
	}
	float3 temp_min = points[index];
	float3 temp_max = points[index + number_of_models_const];
	points[index] = temp_min - F3(collision_envelope_const) + global_origin_const;
	points[index + number_of_models_const] = temp_max + F3(collision_envelope_const) + global_origin_const;
}

ChCCollisionGPU::ChCCollisionGPU() { //Constructor
	number_of_models = 0; //Initialize values
	number_of_contacts_possible = 0;
	do_compute_bounds = true;
}
;
ChCCollisionGPU::~ChCCollisionGPU() {
}
void ChCCollisionGPU::Clear() {
}
void ChCCollisionGPU::TuneCD(int ss, int ee) {
}


void ChCCollisionGPU::ComputeAABB() {
	aabb_data.resize(number_of_models * 2);
Compute_AABBs<<<BLOCKS(number_of_models),THREADS>>>(
		CASTF3(data_container->device_pos_data),
		CASTF4(data_container->device_rot_data),
		CASTF3(data_container->device_ObA_data),
		CASTF3(data_container->device_ObB_data),
		CASTF3(data_container->device_ObC_data),
		CASTF4(data_container->device_ObR_data),
		CASTI3(data_container->device_typ_data),
		CASTF3(aabb_data));

}
void ChCCollisionGPU::ComputeBounds() {
	if (do_compute_bounds) {

		//		if (object_data_host[0].B.w == 0) {
		//			ComputeAABBSphere(object_data_host[0], init_min, init_max);
		//		} else if (object_data_host[0].B.w == 1) {
		//			ComputeAABBTriangle(object_data_host[0], init_min, init_max);
		//		} else if (object_data_host[0].B.w == 2 || object_data_host[0].B.w == 3
		//				|| object_data_host[0].B.w == 4) {
		//			ComputeAABBBox(object_data_host[0], init_min, init_max);
		//		}

		bbox init = bbox(aabb_data[0], aabb_data[0]);
		bbox_transformation unary_op;
		bbox_reduction binary_op;
		bbox result = thrust::transform_reduce(aabb_data.begin(), aabb_data.end(), unary_op, init, binary_op);
		min_bounding_point = result.first;
		max_bounding_point = result.second;

		global_origin = fabs(min_bounding_point); //Determine Global Origin
		max_dimension = max3(global_origin + fabs(max_bounding_point)); //Determine Max point in space
		bin_size_vec = (global_origin + fabs(max_bounding_point)) / (powf(number_of_models * 2, 1 / 3.0));
		bin_size_vec = 1.0 / bin_size_vec;
		do_compute_bounds = false;
	}
}

__global__ void FindGrid(float3* AABBs, uint3* aabb_minmax) {
	uint index = blockIdx.x * blockDim.x + threadIdx.x;
	if (index >= number_of_models_const) {
		return;
	}
	uint3 gmin = Hash(AABBs[index] * 10.0);
	uint3 gmax = Hash(AABBs[index + number_of_models_const] * 10.0);
	aabb_minmax[index] = gmin;
	aabb_minmax[index + number_of_models_const] = gmax;

}
__device__ __host__ bool operator ==(const uint3 &a, const uint3 &b) {
	return ((a.x == b.x) && (a.y == b.y) && (a.z == b.z));
}
void ChCCollisionGPU::UpdateAABB() {

	COPY_TO_CONST_MEM(collision_envelope); //Contact Envelope
	COPY_TO_CONST_MEM(global_origin); //Origin for Physical Space
	COPY_TO_CONST_MEM(bin_size_vec);
	Offset_AABBs<<<BLOCKS(number_of_models),THREADS>>>(CASTF3(aabb_data));

	aabb_min_max_bin.resize(number_of_models * 2);
	FindGrid<<<BLOCKS(number_of_models),THREADS>>>(CASTF3(aabb_data), CASTU3(aabb_min_max_bin));

	if (aabb_min_max_bin.size() == aabb_min_max_bin_old.size()) {
		if (Thrust_Equal(aabb_min_max_bin,aabb_min_max_bin_old)) {
			return;
		}
	}
	do_compute_bounds = true;
	aabb_min_max_bin_old = aabb_min_max_bin;
	Broadphase();
}

void ChCCollisionGPU::Run() {
	COPY_TO_CONST_MEM(number_of_models);
	ComputeAABB();
	ComputeBounds();
	UpdateAABB();
	Narrowphase();
}

void ChCCollisionGPU::AddObject(const float3 &A, const float3 &B, const float3 &C, const float4 &R, const int2 &F, const int3 &T) {
	data_container->host_ObA_data.push_back(A);
	data_container->host_ObB_data.push_back(B);
	data_container->host_ObC_data.push_back(C);
	data_container->host_ObR_data.push_back(R);
	data_container->host_fam_data.push_back(F);
	data_container->host_typ_data.push_back(T);
	number_of_models++;
}
