#pragma once
#ifndef FIXRIGID_H
#define FIXRIGID_H

#undef _XOPEN_SOURCE
#undef _POSIX_C_SOURCE
#include "Python.h"
#include "Fix.h"
#include "FixBond.h"
#undef _XOPEN_SOURCE
#undef _POSIX_C_SOURCE
#include <boost/python.hpp>
#undef _XOPEN_SOURCE
#undef _POSIX_C_SOURCE
#include <boost/python/list.hpp>
#include "GPUArrayDeviceGlobal.h"

void export_FixRigid();

class FixRigid : public Fix {
    private:
        // array holding ids of a given water molecule
        GPUArrayDeviceGlobal<int4> waterIdsGPU;

        // array holding positions
        GPUArrayDeviceGlobal<float4> xs_0;

        // array holding velocities before constraints are applied
        GPUArrayDeviceGlobal<float4> vs_0;


        GPUArrayDeviceGlobal<float4> dvs_0;


        GPUArrayDeviceGlobal<float4> fs_0;


        GPUArrayDeviceGlobal<float4> com;


        GPUArrayDeviceGlobal<float4> fix_len;


        std::vector<int4> waterIds;


        std::vector<BondVariant> bonds;


        std::vector<float4> invMassSums;


        // boolean defaulting to false in the constructor, denoting whether this is TIP4P/2005
        bool TIP4P;

        // boolean defaulting to false in the constructor, denoting whether this is TIP3P
        bool TIP3P;

        // computes the force partition constant for TIP4P for modification of forces on the molecule
        void compute_gamma();

        // the force partition constant to distribute force from M-site to O, H, H atoms
        //  See Feenstra, Hess, and Berendsen, J. Computational Chemistry, Vol. 20, No. 8, 786-798 (1999)
        //  -- specifically, appendix A, expression 6
        float gamma;

    public:


        //! Constructor
        /*!
        * \param state Pointer to the simulation state
        * \param handle "Name" of the Fix
        * \param groupHandle String specifying group of atoms this Fix acts on
        */
        FixRigid(SHARED(State), std::string handle_, std::string groupHandle_);


        //! First half step of the integration
        /*!
         * \return Result of the FixRigid::stepInit() call.
         */
        bool stepInit();

        //! Second half step of the integration
        /*!
         * \return Result of FixRigid::stepFinal() call.
         */
        bool stepFinal();


        //! Prepare FixRigid for simulation run
        bool prepareForRun();


        //! Reset the position of the M-site after integrating the position of the molecule.
        //  -- Note that we do /not/ solve the rigid body constraints at this point;
        //     However, the massless site does not evolve with the other atoms of the molecule (if TIP4P);
        //     So, manually re-set it here for accuracy.
        //     Also, check if this is rigorously correct or a valid approximation. \TODO/
        void handleBoundsChange();

        //! Create a rigid constraint on a TIP3P water molecule
        /*!
         * \ param id_a The atom id in the simulation state of the Oxygen atom in TIP3P
         * \ param id_b The atom id in the simulation state of a Hydrogen atom in TIP3P
         * \ param id_c The atom id in the simulation state of a Hydrogen atom in TIP3P
         */
        void createRigid(int, int, int);


        //! Create a rigid constraint on a TIP4P/2005 water molecule
        /*!
         * \ param id_a The atom id in the simulation state of the Oxygen atom in TIP4P/2005
         * \ param id_b The atom id in the simulation state of a Hydrogen atom in TIP4P/2005
         * \ param id_c The atom id in the simulation state of a Hydrogen atom in TIP4P/2005
         * \ param id_d The atom id in the simulation state of the M site in TIP4P/2005
         */
        void createRigid(int, int, int, int);

       
        std::vector<BondVariant> *getBonds() {
            return &bonds;
  }
};

#endif /* FIXRIGID_H */
