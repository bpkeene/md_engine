#pragma once

#include "BoundsGPU.h"
#include "cutils_func.h"
#include "Virial.h"
#include "helpers.h"
#include "SquareVector.h"
#include "cutils_math.h"
#include <boost/shared_ptr.hpp>

template <class EVALUATOR, bool COMP_VIRIALS> 
__global__ void compute_E3B_force
        (int nMolecules, 
         const int4 *__restrict__ atomsFromMolecule,
         const uint16_t *__restrict__ neighborCounts, 
         const uint *__restrict__ neighborlist, 
         const uint32_t * __restrict__ cumulSumMaxPerBlock, 
         int warpSize, 
         const float4 *__restrict__ xs, 
         float4 *__restrict__ fs, 
         BoundsGPU bounds, 
         Virial *__restrict__ virials,
         EVALUATOR eval)
{

    int idx = GETIDX();

    // might be wise to do NTPA (atom actually being a molecule in this case)
    // -- if we incorporate dynamical parallelism, would that lead to a poorly distributed workload?
    if (idx < nMolecules) {

        // these are atom idxs at a given water molecule idx
        int4 atomsMolecule1 = atomsFromMolecule[idx];

        /* NOTE to others: see the notation used in 
         * Kumar and Skinner, J. Phys. Chem. B., 2008, 112, 8311-8318
         * "Water Simulation Model with Explicit 3 Body Interactions"
         *
         * we use their notation for decomposing the molecules into constituent atoms a,b,c (oxygen, hydrogen, hydrogen)
         * and decomposing the given trimer into the set of molecules 1,2,3 (water molecule 1, 2, and 3)
         */
       
        //int4 atomsMolecule1 = make_int4(idToIdx[atomMolecule.x]....)
        // copy the float4 vectors of the positions
        int idx_a1 = atomsMolecule1.x;
        int idx_b1 = atomsMolecule1.y;
        int idx_c1 = atomsMolecule1.z;

        float4 pos_a1_whole = xs[idx_a1];
        float4 pos_b1_whole = xs[idx_b1];
        float4 pos_c1_whole = xs[idx_c1];

        // now, get just positions in float3
        float3 pos_a1 = make_float3(pos_a1_whole);
        float3 pos_b1 = make_float3(pos_b1_whole);
        float3 pos_c1 = make_float3(pos_c1_whole);
        
        //printf("idx %d molecule id %d oxygen at %f %f %f\n", idx, moleculeId, pos_a1.x, pos_a1.y, pos_a1.z);
        // create a new force sum variable for these atoms
        float3 fs_a1_sum = make_float3(0.0, 0.0, 0.0);
        float3 fs_b1_sum = make_float3(0.0, 0.0, 0.0);
        float3 fs_c1_sum = make_float3(0.0, 0.0, 0.0);
        
        // iterate over the neighbors of molecule '1'; compute the two-body interaction w.r.t all it's neighbors
        // when they are denoted as molecule '2' (or, correspondingly, j)
        
        // number of neighbors this molecule has, with which it can form trimers
        int numNeighMolecules = neighborCounts[idx];

        // so we have our cumulSumMaxPerBlock, warpSize, and 1 thread per 'atom' (this is by molecule, though)
        //int baseIdx = baseNeighlistIdx(cumulSumMaxPerBlock,warpSize,idx); 
        int baseIdx = baseNeighlistIdx(cumulSumMaxPerBlock, warpSize, idx);

        /*
        printf("idx %d: baseIdx: %d\n",idx, baseIdx);
        printf("idx %d: idx_{a1,b1,c1}: %d %d %d\n", idx,idx_a1, idx_b1, idx_c1);
        printf("idx %d: pos_a1_whole: %18.14f %18.14f %18.14f\n",idx,pos_a1_whole.x, pos_a1_whole.y,pos_a1_whole.z);
        printf("idx %d: numNeighMolecules: %d\n",idx, numNeighMolecules);
        */

        for (int j = 0; j < (numNeighMolecules); j++) {
            // get idx of this molecule
            // -- then, the atomIDs that we need are somehow accessible via MoleculeID
            int nlistIdx = baseIdx + warpSize * j;

            uint jIdx = neighborlist[nlistIdx];
            // get the atom idxs from this molecule idx
            int4 atomsMolecule2 = atomsFromMolecule[jIdx];
            // get the atom idxs from the atom ids.. since the per-atom arrays are sorted by idx
            int idx_a2 = atomsMolecule2.x;
            int idx_b2 = atomsMolecule2.y;
            int idx_c2 = atomsMolecule2.z;

            float4 pos_a2_whole = xs[idx_a2];
            float4 pos_b2_whole = xs[idx_b2];
            float4 pos_c2_whole = xs[idx_c2];
    
            // XXX TO DELETE
            /*
            printf("idx %d:  nlistIdx, jIdxRaw, moleculeId2: %d %d %d\n",idx,nlistIdx,jIdxRaw,moleculeId2);
            printf("idx %d:  atomsMolecule2: %d %d %d\n",idx,atomsMolecule2.x, atomsMolecule2.y, atomsMolecule2.z);
            printf("idx %d:  idx_a2, idx_b2, idx_c2: %d %d %d\n",idx, idx_a2, idx_b2, idx_c2);
            printf("idx %d:  pos_a2_whole: %18.14f %18.14f %18.14f\n",idx,  pos_a2_whole.x, pos_a2_whole.y, pos_a2_whole.z);
            */

            // here we should extract the positions for the O, H atoms of this water molecule
            float3 pos_a2 = make_float3(pos_a2_whole);
            float3 pos_b2 = make_float3(pos_b2_whole);
            float3 pos_c2 = make_float3(pos_c2_whole);

            // we have four OH distances to compute here
            
            // -- just as the paper does, we compute the vector w.r.t. the hydrogen,
            //    but on molecule 1.
            
            float3 r_b2a1 = bounds.minImage(pos_b2 - pos_a1);
            float3 r_c2a1 = bounds.minImage(pos_c2 - pos_a1);
            
            float3 r_b1a2 = bounds.minImage(pos_b1 - pos_a2);
            float3 r_c1a2 = bounds.minImage(pos_c1 - pos_a2);

            float r_b2a1_magnitude = length(r_b2a1);
            float r_c2a1_magnitude = length(r_c2a1);
            float r_b1a2_magnitude = length(r_b1a2);
            float r_c1a2_magnitude = length(r_c1a2);

            // we now have our molecule 'j'
            // compute the two-body correction term w.r.t the oxygens
            float3 r_a1a2 = bounds.minImage(pos_a1 - pos_a2);
            float r_a1a2_magnitude = length(r_a1a2);

            fs_a1_sum += eval.twoBodyForce(r_a1a2,r_a1a2_magnitude);
           
            // compute the number of O-H distances computed so far that are within the range of the three-body cutoff
            // note: order really doesn't matter here; just checking if (val < 5.2 Angstroms)
            //

            int numberOfDistancesWithinCutoff = eval.getNumberWithinCutoff(r_b2a1_magnitude,
                                                                           r_c2a1_magnitude,
                                                                           r_b1a2_magnitude,
                                                                           r_c1a2_magnitude);

            // save this for the k loop
            int ijNDist = numberOfDistancesWithinCutoff;
            // compute the exponential force scalar resulting from the a1b2, a1c2, a2b1, a2c1 contributions,
            // so that we don't have to compute these in the k-molecule loop
            // compute the exponential factors (without the prefactor)
            // -- we send to eval rather than computing the exponential here b/c we don't have the constant here
            
            float fs_b2a1_scalar = eval.threeBodyForceScalar(r_b2a1_magnitude);
            float fs_c2a1_scalar = eval.threeBodyForceScalar(r_c2a1_magnitude);
            float fs_b1a2_scalar = eval.threeBodyForceScalar(r_b1a2_magnitude);
            float fs_c1a2_scalar = eval.threeBodyForceScalar(r_c1a2_magnitude);
            
            // --> get molecule 'k' to complete the trimer

            // we only wish to compute $-/nabla E_{ijk}$ for all unique combos of trimers, so this should range 
            // from k = j+1, while still less than numNeighMolecules w.r.t. baseMolecule ('i');
            
            for (int k = j+1; k < numNeighMolecules; k++) {
 
                // reset to initial ij value; we use += below, which would lead to accumulatino within the k-loop
                numberOfDistancesWithinCutoff = ijNDist;

                // grab warp index corresponding to this 'k'
                int klistMoleculeIdx = baseIdx + warpSize * k;

                // grab the molecule idx at this index of the neighborlist
                uint kIdx = neighborlist[klistMoleculeIdx];

                int4 atomsMolecule3 = atomsFromMolecule[kIdx];

                int idx_a3 = atomsMolecule3.x;
                int idx_b3 = atomsMolecule3.y;
                int idx_c3 = atomsMolecule3.z;

                // extract positions of O, H atoms of this water molecule
                float4 pos_a3_whole = xs[idx_a3];
                float4 pos_b3_whole = xs[idx_b3];
                float4 pos_c3_whole = xs[idx_c3];
                
                /*
                printf("idx %d:    klistMoleculeIdx, krawIdx, moleculeId3: %d %d %d\n", idx, klistMoleculeIdx, krawIdx, moleculeId3);
                printf("idx %d:    atomsMolecule3: %d %d %d\n", idx, atomsMolecule3.x, atomsMolecule3.y, atomsMolecule3.z);
                printf("idx %d:    idx_{a3, b3, c3}: %d %d %d\n",idx, idx_a3, idx_b3, idx_c3);
                printf("idx %d:    pos_a3_whole: %18.14f %18.14f %18.14f\n", idx,pos_a3_whole.x, pos_a3_whole.y, pos_a3_whole.z);
                */

                float3 pos_a3 = make_float3(pos_a3_whole);
                float3 pos_b3 = make_float3(pos_b3_whole);
                float3 pos_c3 = make_float3(pos_c3_whole);
                
                // compute the pertinent O-H distances for use in our potential (there are 8 that we have yet to compute)
                // -- distances vector for b3a1 and c3a1; a1 reference atom
                float3 r_b3a1 = bounds.minImage(pos_b3 - pos_a1);
                float3 r_c3a1 = bounds.minImage(pos_c3 - pos_a1);
               
                // -- distances vector for b3a2 and c3a2; a2 reference atom
                float3 r_b3a2 = bounds.minImage(pos_b3 - pos_a2);
                float3 r_c3a2 = bounds.minImage(pos_c3 - pos_a2);

                // -- distances vector for b1a3 and c1a3; a3 reference atom
                float3 r_b1a3 = bounds.minImage(pos_b1 - pos_a3);
                float3 r_c1a3 = bounds.minImage(pos_c1 - pos_a3);

                // -- distance vector for b2a3 and c2a3; a3 reference atom
                float3 r_b2a3 = bounds.minImage(pos_b2 - pos_a3);
                float3 r_c2a3 = bounds.minImage(pos_c2 - pos_a3);
               
                /*
                 *  get the magnitude of the new distance vectors, and check if we still need to compute this potential
                 *  (i.e., see if this is a valid trimer, that there will be some non-zero threebody contribution)
                 */

                float r_b3a1_magnitude = length(r_b3a1);
                float r_c3a1_magnitude = length(r_c3a1);
                
                float r_b3a2_magnitude = length(r_b3a2);
                float r_c3a2_magnitude = length(r_c3a2);

                float r_b1a3_magnitude = length(r_b1a3);
                float r_c1a3_magnitude = length(r_c1a3);
                float r_b2a3_magnitude = length(r_b2a3);
                float r_c2a3_magnitude = length(r_c2a3);
    
                //printf("line 236 of compute_e3b3\n");
                // compute the number of additional distances within the cutoff;
                // if the total is >= 2, we need to compute the force terms.
                numberOfDistancesWithinCutoff += eval.getNumberWithinCutoff(r_b3a1_magnitude,
                                                                            r_c3a1_magnitude,
                                                                            r_b3a2_magnitude,
                                                                            r_c3a2_magnitude);

                numberOfDistancesWithinCutoff += eval.getNumberWithinCutoff(r_b1a3_magnitude,
                                                                            r_c1a3_magnitude,
                                                                            r_b2a3_magnitude,
                                                                            r_c2a3_magnitude);

                // if there is only 1 intermolecular O-H distance within the cutoff, all terms will be zero
                if (numberOfDistancesWithinCutoff > 1) {
                    // send our forces sum variable, the distance vectors, and their corresponding magnitude to the force evaluate function
                    // -- also, for speed, we pre-compute the force scalar corresponding to the a1b2, a1c2, a2b1, and a2c1 distances
                    // -- then, we are done
                    // 
                    // this is a long parameter list, but its kind of necessary... so. Could group in a struct, but 
                    // this is explicit. 
                    // compute the exponential force scalar resulting from the a1b2, a1c2, a2b1, a2c1 contributions,
                   
    

                    //printf("calling threeBodyForce!\n");
                    eval.threeBodyForce(fs_a1_sum, fs_b1_sum, fs_c1_sum,
                                        fs_b2a1_scalar, fs_c2a1_scalar,
                                        fs_b1a2_scalar, fs_c1a2_scalar,
                                        r_b2a1, r_b2a1_magnitude,
                                        r_c2a1, r_c2a1_magnitude,
                                        r_b3a1, r_b3a1_magnitude,
                                        r_c3a1, r_c3a1_magnitude,
                                        r_b1a2, r_b1a2_magnitude,
                                        r_c1a2, r_c1a2_magnitude,
                                        r_b3a2, r_b3a2_magnitude,
                                        r_c3a2, r_c3a2_magnitude,
                                        r_b1a3, r_b1a3_magnitude,
                                        r_c1a3, r_c1a3_magnitude, 
                                        r_b2a3, r_b2a3_magnitude, 
                                        r_c2a3, r_c2a3_magnitude);
                 
                    /*
                    printf("fs_a1_sum.{x,y,z}: %18.14f %18.14f %18.14f\n",fs_a1_sum.x,fs_a1_sum.y,fs_a1_sum.z);
                    printf("fs_b1_sum.{x,y,z}: %18.14f %18.14f %18.14f\n",fs_b1_sum.x,fs_b1_sum.y,fs_b1_sum.z);
                    printf("fs_c1_sum.{x,y,z}: %18.14f %18.14f %18.14f\n",fs_c1_sum.x,fs_c1_sum.y,fs_c1_sum.z);
                    */
                } // end if (numberOfDistancesWithinCutoff >= 2)
            } // end for (int k = j+1; k < numNeighMolecules; k++) 
        } // end for (int j = 0; j < (numNeighMolecules); j++) 
        

        // we now have the aggregate force sums for the three atoms a1, b1, c1; add them to the actual atoms data
        // idx_a1 is idx of oxygen for molecule i
        // idx_b1 is idx of hydrogen 1 for molecule i
        // idx_c1 is idx of hydrogen 2 for molecule i
        float4 fs_a1_whole = fs[idx_a1];
        float4 fs_b1_whole = fs[idx_b1];
        float4 fs_c1_whole = fs[idx_c1];

        fs_a1_whole += fs_a1_sum;
        fs_b1_whole += fs_b1_sum;
        fs_c1_whole += fs_c1_sum;

        fs[idx_a1] = fs_a1_whole;
        fs[idx_b1] = fs_b1_whole;
        fs[idx_c1] = fs_c1_whole;

    } // end if (idx < nMolecules) 
} // end function compute

// and same thing for energy kernel computation... 


template <class EVALUATOR, bool COMP_VIRIALS> 
__global__ void compute_E3B_energy
        (int nMolecules, 
         const int *__restrict__ molIdToIdxs,
         const uint *__restrict__ waterMolecIds,
         const int4 *__restrict__ atomsFromMolecule,
         const uint16_t *__restrict__ neighborCounts, 
         const uint *__restrict__ neighborlist, 
         const uint32_t * __restrict__ cumulSumMaxPerBlock, 
         int warpSize, 
         const int *__restrict__ idToIdxs,
         const float4 *__restrict__ xs, 
         float4 *__restrict__ fs, 
         BoundsGPU bounds, 
         Virial *__restrict__ virials,
         EVALUATOR eval)

{
    //int idx = GETIDX();
    return;
}

