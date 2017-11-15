#include "IntegratorRelax.h"
#include "cutils_func.h"
#include "State.h"

#include "Fix.h"
using namespace MD_ENGINE;

IntegratorRelax::IntegratorRelax(SHARED(State) state_)
    : Integrator(state_.get())
{
    //FIRE parameters
    alphaInit = 0.1;
    alphaShrink = 0.99;
    dtGrow = 1.1;
    dtShrink = 0.5;
    delay = 5;
    dtMax_mult = 10;
}

//kernels for FIRE relax
//VDotF by hand
__global__ void vdotF_cu(real *dest, real4 *vs,real4 *fs, int n) {
    extern __shared__ real tmp[];  // should have length of # threads in a block (PERBLOCK)
    int potentialIdx = blockDim.x*blockIdx.x + threadIdx.x;
    if (potentialIdx < n) {
        tmp[threadIdx.x] = dot(make_real3(vs[blockDim.x*blockIdx.x + threadIdx.x]),
                               make_real3(fs[blockDim.x*blockIdx.x + threadIdx.x]));
    } else {
        tmp[threadIdx.x] = 0;
    }
    __syncthreads();
    int maxLookahead = log2f(blockDim.x-1);
    for (int i=0; i<=maxLookahead; i++) {
        int curLookahead = powf(2, i);
        if (! (threadIdx.x % (curLookahead*2))) {
            tmp[threadIdx.x] += tmp[threadIdx.x + curLookahead];
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        atomicAdd(dest, tmp[0]);
    }
}

//update velocities
__global__ void FIRE_new_vel_cu(int nAtoms, real4 *vs, real4 *fs, real scale1, real scale2) {
    int idx = GETIDX();
    if (idx < nAtoms) {
        real4 vel = vs[idx];
        real4 force = fs[idx];
        real invmass = vel.w;
        real4 newVel = vel*scale1 + force*scale2;
        newVel.w = invmass;
        vs[idx] = newVel;
    }
}

//zero velocities
__global__ void zero_vel_cu(int nAtoms, real4 *vs) {
    int idx = GETIDX();
    if (idx < nAtoms) {
        real4 vel = vs[idx];
        vs[idx] = make_real4(0.0f,0.0f,0.0f,vel.w);
    }
}

//MD step
__global__ void FIRE_preForce_cu(int nAtoms, real4 *xs, real4 *vs, real4 *fs, real dt, real dtf) {
    int idx = GETIDX();
    if (idx < nAtoms) {


        real4 vel = vs[idx];
        real4 force = fs[idx];

        real invmass = vel.w;
        real groupTag = force.w;
        xs[idx] = xs[idx] + make_real3(vel) * dt;
        real3 newVel = make_real3(force) * dtf * invmass;
        vs[idx] = vel + newVel;
        fs[idx] = make_real4(0, 0, 0, groupTag);
    }
}




double IntegratorRelax::run(int numTurns, double fTol) {
    std::cout << "FIRE relaxation\n";
    
    basicPreRunChecks();

    // basicPrepare now only handles State prepare and sending global State data to device
    basicPrepare(numTurns);

    // prepare the fixes that do not require forces to be computed
    prepareFixes(false);
    
    force(true);

    // prepare the fixes that require forces to be computed on instantiation;
    // --- we also handle the datacomputers here, now that all information is available
    //     (e.g., for tempComputers, rigid fix will be able to quantify the reduction in DOF)
    prepareFixes(true);

    // finally, prepare barostats, thermostats, datacomputers, etc.
    // now that pair potentials, electrostatics, and constraints are prepared.
    // -- should we prepare the datacomputers first? possibly..
    prepareFinal();


    CUT_CHECK_ERROR("FIRE relaxation init failed");  // Debug feature, checks error code

    //initial  values
    int lastNegative = 0;
    double dt = state->dt;
    double alpha = alphaInit;
    const double dtMax = dtMax_mult * dt;

    int warpSize = state->devManager.prop.warpSize;

    //assuming constant number of atoms during run
    int atomssize = state->atoms.size();
    int periodicInterval = state->periodicInterval;
    int nblock = NBLOCK(atomssize);
    int remainder = state->turn % periodicInterval;
    int turnInit = state->turn; 

    //set velocity to 0
    // 	state->gpd.vs.memsetByVal(make_real3(0.0f,0.0f,0.0f);
    zero_vel_cu<<<nblock, PERBLOCK>>>(atomssize,state->gpd.vs.getDevData());
    CUT_CHECK_ERROR("zero_vel_cu kernel execution failed");

    //vars to store kernels outputs
    GPUArrayGlobal<real>VDotV(1);
    GPUArrayGlobal<real>VDotF(1);
    GPUArrayGlobal<real>FDotF(1);
    GPUArrayGlobal<real>force(1);

    //neighborlist build
    state->gridGPU.periodicBoundaryConditions(-1, true);
    DataManager &dataManager = state->dataManager;
    for (int i=0; i<numTurns; i++) {
        checkQuit();
        //init to 0 on cpu and gpu
        VDotV.memsetByVal(0.0);
        VDotF.memsetByVal(0.0);
        FDotF.memsetByVal(0.0);
        int virialMode = dataManager.getVirialModeForTurn(state->turn);
        stepInit(virialMode==1 or virialMode==2);
        //vdotF calc
        if (! ((remainder + i) % periodicInterval)) {
            state->gridGPU.periodicBoundaryConditions();
        }
        asyncOperations();

        vdotF_cu <<<nblock,PERBLOCK,sizeof(real)*PERBLOCK>>>(
                    VDotF.getDevData(),
                    state->gpd.vs.getDevData(),
                    state->gpd.fs.getDevData(),
                    atomssize);
        CUT_CHECK_ERROR("vdotF_cu kernel execution failed");
        VDotF.dataToHost();

        if (VDotF.h_data[0] > 0) {
            //VdotV calc
            accumulate_gpu<real, real4, SumVectorSqr3D, N_DATA_PER_THREAD> <<<NBLOCK(atomssize / (double) N_DATA_PER_THREAD), PERBLOCK, N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>> 
                (
                 VDotV.getDevData(),
                 state->gpd.vs.getDevData(),
                 atomssize,
                 warpSize,
                 SumVectorSqr3D()
                );


            /*
            sumVectorSqr3D<real,real4, N_DATA_PER_THREAD> <<<NBLOCK(atomssize/(double)N_DATA_PER_THREAD),PERBLOCK,N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>>(
                                            VDotV.getDevData(),
                                            state->gpd.vs.getDevData(),
                                            atomssize,
                                            warpSize);
                                            */
            CUT_CHECK_ERROR("vdotV_cu kernel execution failed");
            VDotV.dataToHost();

            //FdotF
            accumulate_gpu<real, real4, SumVectorSqr3D, N_DATA_PER_THREAD> <<<NBLOCK(atomssize / (double) N_DATA_PER_THREAD), PERBLOCK, N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>> 
                (
                 FDotF.getDevData(),
                 state->gpd.fs.getDevData(),
                 atomssize,
                 warpSize,
                 SumVectorSqr3D()
                );

            /*
            sumVectorSqr3D<real,real4, N_DATA_PER_THREAD> <<<NBLOCK(atomssize/(double)N_DATA_PER_THREAD),PERBLOCK,N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>>(
                                            FDotF.getDevData(),
                                            state->gpd.fs.getDevData(),
                                            atomssize,
                                            warpSize);
                                            */
            CUT_CHECK_ERROR("fdotF_cu kernel execution failed");
            FDotF.dataToHost();

            real scale1 = 1 - alpha;
            real scale2 = 0;
            cudaDeviceSynchronize();
            if (FDotF.h_data[0] != 0) {
                scale2 = alpha * sqrt(VDotV.h_data[0] / FDotF.h_data[0]);
            }
            //set velocity to
            //a.vel = a.vel * scale1 + a.force * scale2;
            FIRE_new_vel_cu <<<nblock, PERBLOCK>>>(
                                atomssize,
                                state->gpd.vs.getDevData(),
                                state->gpd.fs.getDevData(),
                                scale1,scale2);
            //check number of steps since negative 
            if (i - lastNegative > delay) {
                dt = fmin(dt*dtGrow, dtMax);
                alpha *= alphaShrink;

            }
        } else {
            lastNegative = i;
            dt *= dtShrink;
            alpha = alphaInit;
            //set velocity to 0
            //state->gpd.vs.memsetByVal(make_real3(0.0f,0.0f,0.0f);
            zero_vel_cu <<<nblock, PERBLOCK>>>(atomssize, state->gpd.vs.getDevData());
            CUT_CHECK_ERROR("zero_vel_cu kernel execution failed");

        }

        FIRE_preForce_cu <<<nblock, PERBLOCK>>>(
                            atomssize,
                            state->gpd.xs.getDevData(),
                            state->gpd.vs.getDevData(),
                            state->gpd.fs.getDevData(),
                            dt, dt*state->units.ftm_to_v);
        CUT_CHECK_ERROR("FIRE_preForce_cu kernel execution failed");

        Integrator::forceSingle(virialMode);

        if (fTol > 0 and i > delay and not (i%delay)) { //only check every so often
            //total force calc
            force.memsetByVal(0.0);
            accumulate_gpu<real, real4, SumVectorSqr3D, N_DATA_PER_THREAD> <<<NBLOCK(atomssize/(double)N_DATA_PER_THREAD),PERBLOCK,N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>> 
                (
                 force.getDevData(),
                 state->gpd.fs.getDevData(),
                 atomssize,
                 warpSize,
                 SumVectorSqr3D()
                );
            /*
            sumVectorSqr3D<real,real4, N_DATA_PER_THREAD> <<<NBLOCK(atomssize/(double)N_DATA_PER_THREAD),PERBLOCK,N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>>(
                                        force.getDevData(),
                                        state->gpd.fs.getDevData(),
                                        atomssize,
                                        warpSize);
                                        */
            CUT_CHECK_ERROR("kernel execution failed");//Debug feature, check error code

            force.dataToHost();
            //std::cout<<"Fire relax: force="<<force<<"; turns="<<i<<'\n';
            cudaDeviceSynchronize();

            if (force.h_data[0] < fTol*fTol) {//tolerance achived, exting
                basicFinish();
                real finalForce = sqrt(force.h_data[0]);
                std::cout<<"FIRE relax done: force="<< finalForce <<"; turns="<<i+1<<'\n';
                return finalForce;
            }
        } 

        //shout status
        if (state->verbose and not ((state->turn - turnInit) % state->shoutEvery)) {
            std::cout << "Turn " << (int) state->turn 
                      << " " << (int) (100 * (state->turn - turnInit) / (double) numTurns)
                      << " percent done" << std::endl;
        }
        dataManager.clearVirialTurn(state->turn);
        state->turn++;

    }
    //total force calculation
    force.memsetByVal(0.0);
    accumulate_gpu<real, real4, SumVectorSqr3D, N_DATA_PER_THREAD> <<<NBLOCK(atomssize/(double)N_DATA_PER_THREAD),PERBLOCK,N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>> 
        (
         force.getDevData(),
         state->gpd.fs.getDevData(),
         atomssize,
         warpSize,
         SumVectorSqr3D()
        );
    /*
    sumVectorSqr3D<real,real4, N_DATA_PER_THREAD> <<<NBLOCK(atomssize/(double)N_DATA_PER_THREAD),PERBLOCK,N_DATA_PER_THREAD*sizeof(real)*PERBLOCK>>>(
                                  force.getDevData(),
                                  state->gpd.fs.getDevData(),
                                  atomssize,
                                  warpSize);
                                  */
    CUT_CHECK_ERROR("kernel execution failed"); //Debug feature, check error code

    basicFinish();
    cudaDeviceSynchronize();
    real finalForce = sqrt(force.h_data[0]) / atomssize;
    std::cout << "FIRE relax done: force=" << finalForce 
              << "; turns=" << numTurns << std::endl;

    return finalForce;
}

void export_IntegratorRelax() {
    boost::python::class_<IntegratorRelax,
                          SHARED(IntegratorRelax),
                          boost::python::bases<Integrator>,
                          boost::noncopyable >(
            "IntegratorRelax",
            boost::python::init<SHARED(State)>()
    )
    .def("run", &IntegratorRelax::run)
    .def("set_params", &IntegratorRelax::set_params,
            (boost::python::arg("alphaInit")=-1,
             boost::python::arg("alphaShrink")=-1,
             boost::python::arg("dtGrow")=-1,
             boost::python::arg("dtShrink")=-1,
             boost::python::arg("delay")=-1,
             boost::python::arg("dtMax_mult")=-1)
    )
    ;
}

