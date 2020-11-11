// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2016 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Hammad Mazhar, Radu Serban
// =============================================================================
#include "chrono/utils/ChUtilsInputOutput.h"
#include "chrono_parallel/ChConfigParallel.h"
#include "chrono_parallel/physics/ChSystemParallel.h"
#include "chrono_parallel/solver/ChSolverParallel.h"
#include "chrono_parallel/solver/ChIterativeSolverParallel.h"
#include "chrono_parallel/collision/ChContactContainerParallelNSC.h"
#include "chrono_parallel/collision/ChCollisionSystemParallel.h"
#include "chrono_parallel/collision/ChCollisionSystemBulletParallel.h"

using namespace chrono;

ChSystemParallelNSC::ChSystemParallelNSC() : ChSystemParallel() {
    contact_container = chrono_types::make_shared<ChContactContainerParallelNSC>(data_manager);
    contact_container->SetSystem(this);

    solver = chrono_types::make_shared<ChIterativeSolverParallelNSC>(data_manager);

    // Set this so that the CD can check what type of system it is (needed for narrowphase)
    data_manager->settings.system_type = SystemType::SYSTEM_NSC;

    data_manager->system_timer.AddTimer("ChSolverParallel_solverA");
    data_manager->system_timer.AddTimer("ChSolverParallel_solverB");
    data_manager->system_timer.AddTimer("ChSolverParallel_solverC");
    data_manager->system_timer.AddTimer("ChSolverParallel_solverD");
    data_manager->system_timer.AddTimer("ChSolverParallel_solverE");
    data_manager->system_timer.AddTimer("ChSolverParallel_solverF");
    data_manager->system_timer.AddTimer("ChSolverParallel_solverG");
    data_manager->system_timer.AddTimer("ChSolverParallel_Project");
    data_manager->system_timer.AddTimer("ChSolverParallel_Solve");
    data_manager->system_timer.AddTimer("ShurProduct");
    data_manager->system_timer.AddTimer("ChIterativeSolverParallel_D");
    data_manager->system_timer.AddTimer("ChIterativeSolverParallel_E");
    data_manager->system_timer.AddTimer("ChIterativeSolverParallel_R");
    data_manager->system_timer.AddTimer("ChIterativeSolverParallel_N");
}

ChSystemParallelNSC::ChSystemParallelNSC(const ChSystemParallelNSC& other) : ChSystemParallel(other) {
    //// TODO
}

void ChSystemParallelNSC::ChangeSolverType(SolverType type) {
    std::static_pointer_cast<ChIterativeSolverParallelNSC>(solver)->ChangeSolverType(type);
}

void ChSystemParallelNSC::Add3DOFContainer(std::shared_ptr<Ch3DOFContainer> container) {
    if (auto fea_container = std::dynamic_pointer_cast<ChFEAContainer>(container)) {
        data_manager->fea_container = fea_container;
    } else {
        data_manager->node_container = container;
    }

    container->SetSystem(this);
    container->data_manager = data_manager;
}

void ChSystemParallelNSC::AddMaterialSurfaceData(std::shared_ptr<ChBody> newbody) {
    // Reserve space for material properties for the specified body.
    // Notes:
    //  - the actual data is set in UpdateMaterialProperties()
    //  - coefficients of sliding friction are only needed for fluid-rigid and FEA-rigid contacts;
    //    for now, we store a single value per body (corresponding to the first collision shape,
    //    if any, in the associated collision model)
    data_manager->host_data.sliding_friction.push_back(0);
    data_manager->host_data.cohesion.push_back(0);
}

void ChSystemParallelNSC::UpdateMaterialSurfaceData(int index, ChBody* body) {
    custom_vector<float>& friction = data_manager->host_data.sliding_friction;
    custom_vector<float>& cohesion = data_manager->host_data.cohesion;

    if (body->GetCollisionModel() && body->GetCollisionModel()->GetNumShapes() > 0) {
        auto mat =
            std::static_pointer_cast<ChMaterialSurfaceNSC>(body->GetCollisionModel()->GetShape(0)->GetMaterial());
        friction[index] = mat->GetKfriction();
        cohesion[index] = mat->GetCohesion();
    }
}

void ChSystemParallelNSC::CalculateContactForces() {
    uint num_unilaterals = data_manager->num_unilaterals;
    uint num_rigid_dof = data_manager->num_rigid_bodies * 6;
    uint num_contacts = data_manager->num_rigid_contacts;
    DynamicVector<real>& Fc = data_manager->host_data.Fc;

    data_manager->Fc_current = true;

    if (num_contacts == 0) {
        Fc.resize(6 * data_manager->num_rigid_bodies);
        Fc = 0;
        return;
    }

    LOG(INFO) << "ChSystemParallelNSC::CalculateContactForces() ";

    const SubMatrixType& D_u = blaze::submatrix(data_manager->host_data.D, 0, 0, num_rigid_dof, num_unilaterals);
    DynamicVector<real> gamma_u = blaze::subvector(data_manager->host_data.gamma, 0, num_unilaterals);
    Fc = D_u * gamma_u / data_manager->settings.step_size;
}

real3 ChSystemParallelNSC::GetBodyContactForce(uint body_id) const {
    assert(data_manager->Fc_current);
    return real3(data_manager->host_data.Fc[body_id * 6 + 0], data_manager->host_data.Fc[body_id * 6 + 1],
                 data_manager->host_data.Fc[body_id * 6 + 2]);
}

real3 ChSystemParallelNSC::GetBodyContactTorque(uint body_id) const {
    assert(data_manager->Fc_current);
    return real3(data_manager->host_data.Fc[body_id * 6 + 3], data_manager->host_data.Fc[body_id * 6 + 4],
                 data_manager->host_data.Fc[body_id * 6 + 5]);
}

static inline chrono::ChVector<real> ToChVector(const real3& a) {
    return chrono::ChVector<real>(a.x, a.y, a.z);
}

void ChSystemParallelNSC::SolveSystem() {
    data_manager->system_timer.Reset();
    data_manager->system_timer.start("step");

    Setup();

    data_manager->system_timer.start("update");
    Update();
    data_manager->system_timer.stop("update");

    data_manager->system_timer.start("collision");
    collision_system->Run();
    collision_system->ReportContacts(this->contact_container.get());
    data_manager->system_timer.stop("collision");
    data_manager->system_timer.start("advance");
    std::static_pointer_cast<ChIterativeSolverParallelNSC>(solver)->RunTimeStep();
    data_manager->system_timer.stop("advance");
    data_manager->system_timer.stop("step");
}

void ChSystemParallelNSC::AssembleSystem() {
    //// TODO: load colliding shape information in icontact? Not really needed here.

    Setup();

    collision_system->Run();
    collision_system->ReportContacts(contact_container.get());
    ChSystem::Update();
    contact_container->BeginAddContact();
    chrono::collision::ChCollisionInfo icontact;
    for (int i = 0; i < (signed)data_manager->num_rigid_contacts; i++) {
        vec2 cd_pair = data_manager->host_data.bids_rigid_rigid[i];
        icontact.modelA = Get_bodylist()[cd_pair.x]->GetCollisionModel().get();
        icontact.modelB = Get_bodylist()[cd_pair.y]->GetCollisionModel().get();
        icontact.vN = ToChVector(data_manager->host_data.norm_rigid_rigid[i]);
        icontact.vpA =
            ToChVector(data_manager->host_data.cpta_rigid_rigid[i] + data_manager->host_data.pos_rigid[cd_pair.x]);
        icontact.vpB =
            ToChVector(data_manager->host_data.cptb_rigid_rigid[i] + data_manager->host_data.pos_rigid[cd_pair.y]);
        icontact.distance = data_manager->host_data.dpth_rigid_rigid[i];
        icontact.eff_radius = data_manager->host_data.erad_rigid_rigid[i];
        contact_container->AddContact(icontact);
    }
    contact_container->EndAddContact();

    // Reset sparse representation accumulators.
    for (auto& link : Get_linklist()) {
        link->ConstraintsBiReset();
    }
    for (auto& body : Get_bodylist()) {
        body->VariablesFbReset();
    }
    contact_container->ConstraintsBiReset();

    // Fill in the sparse system representation by looping over all links, bodies,
    // and other physics items.
    double F_factor = step;
    double K_factor = step * step;
    double R_factor = step;
    double M_factor = 1;
    double Ct_factor = 1;
    double C_factor = 1 / step;

    for (auto& link : Get_linklist()) {
        link->ConstraintsBiLoad_C(C_factor, max_penetration_recovery_speed, true);
        link->ConstraintsBiLoad_Ct(Ct_factor);
        link->VariablesQbLoadSpeed();
        link->VariablesFbIncrementMq();
        link->ConstraintsLoadJacobians();
        link->ConstraintsFbLoadForces(F_factor);
    }

    for (int ip = 0; ip < Get_bodylist().size(); ++ip) {
        std::shared_ptr<ChBody> Bpointer = Get_bodylist()[ip];

        Bpointer->VariablesFbLoadForces(F_factor);
        Bpointer->VariablesQbLoadSpeed();
        Bpointer->VariablesFbIncrementMq();
    }

    for (auto& item : Get_otherphysicslist()) {
        item->VariablesFbLoadForces(F_factor);
        item->VariablesQbLoadSpeed();
        item->VariablesFbIncrementMq();
        item->ConstraintsBiLoad_C(C_factor, max_penetration_recovery_speed, true);
        item->ConstraintsBiLoad_Ct(Ct_factor);
        item->ConstraintsLoadJacobians();
        item->KRMmatricesLoad(K_factor, R_factor, M_factor);
        item->ConstraintsFbLoadForces(F_factor);
    }

    contact_container->ConstraintsBiLoad_C(C_factor, max_penetration_recovery_speed, true);
    contact_container->ConstraintsFbLoadForces(F_factor);
    contact_container->ConstraintsLoadJacobians();

    // Inject all variables and constraints into the system descriptor.
    descriptor->BeginInsertion();
    for (auto& body : Get_bodylist()) {
        body->InjectVariables(*descriptor);
    }
    for (auto& link : Get_linklist()) {
        link->InjectConstraints(*descriptor);
    }
    contact_container->InjectConstraints(*descriptor);
    descriptor->EndInsertion();
}

void ChSystemParallelNSC::Initialize() {
    // Mpm update is special because it computes the number of nodes that we have
    // data_manager->node_container->ComputeDOF();

    Setup();

    data_manager->fea_container->Initialize();

    data_manager->system_timer.start("update");
    Update();
    data_manager->system_timer.stop("update");

    data_manager->system_timer.start("collision");
    collision_system->Run();
    collision_system->ReportContacts(this->contact_container.get());
    data_manager->system_timer.stop("collision");

    data_manager->node_container->Initialize();
}

bool ChSystemParallelNSC::Par_Gran_Outhelper(ChSystemParallelNSC* system, const std::string& filename) {
    // Create the CSV stream.
    chrono::utils::CSV_writer csv(" ");
    std::string tab("    ");

    csv << "id,pos_x,pos_y,pos_z,pos_dt_x,pos_dt_y,pos_dt_z,fx,fy,fz"<<std::endl;
    for (auto body : system->Get_bodylist()) {
        ChVector<double>& pos = body->GetPos();
        ChVector<double>& pos_dt = body->GetPos_dt();
        std::cout<<"test_point_1"<<std::endl;
        real3 cont_f;
        //std::cout<<"length: "<<data_manager->host_data.ct_body_map.size()<<std::endl;
        //std::cout<<"BODY length: "<<system->Get_bodylist().size()<<std::endl;
        int index = -1;
        //if(data_manager->host_data.ct_body_map.size()==0){
        //    index = data_manager->host_data.ct_body_map[body->GetId()];
        //}
        std::cout<<"test_point_2"<<std::endl;
        if (index == -1)
            cont_f = real3(0);
        else
            cont_f = data_manager->host_data.ct_body_force[index];
        std::cout<<"test_point_3"<<std::endl;
        csv<<body->GetId()<<","<<(float)pos.x()<<","<<(float)pos.y()<<","<< (float)pos.z()<<","<<(float)pos_dt.x()<<","<<(float)pos_dt.y()<<","<<(float)pos_dt.z()<<","<<(double)cont_f.x<<","<<(double)cont_f.y<<","<<(double)(cont_f.z)<<std::endl;
        std::cout<<"test_point_4"<<std::endl;
    }
    csv.write_to_file(filename);

    return true;
}

