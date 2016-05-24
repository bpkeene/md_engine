#pragma once
#ifndef EVALUATOR_DIHEDRAL_OPLS
#define EVALUATOR_DIHEDRAL_OPLS

#include "cutils_math.h"
#include "Dihedral.h"
#define EPSILON 0.00001f
class DihedralEvaluatorOPLS {
    public:
        //dihedralType, phi, c, scValues, invLenSqrs, c12Mags, c0,

                //float3 myForce = evaluator.force(dihedralType, phi, c, scValues, invLenSqrs, c12Mags, c0, c, invMagProds, c12Mags, invLens, directors, myIdxInDihedral);
        inline __device__ float3 force(DihedralOPLSType dihedralType, float phi, float scValues[3], float invLenSqrs[3], float c12Mangs[3], float c0, float c, float invMagProds[2], float c12Mags[2], float invLens[3], float3 directors[3], int myIdxInDihedral) {
            float3 myForce;
            float sinPhi = sinf(phi);
            float absSinPhi = sinPhi < 0 ? -sinPhi : sinPhi;
            if (absSinPhi < EPSILON) {
                sinPhi = EPSILON;
            }
            float invSinPhi = 1.0f / sinPhi;

            float derivOfPotential = 0.5 * (
                    dihedralType.coefs[0] 
                    - 2.0f * dihedralType.coefs[1] * sinf(2.0f*phi) * invSinPhi
                    + 3.0f * dihedralType.coefs[2] * sinf(3.0f*phi) * invSinPhi
                    - 4.0f * dihedralType.coefs[3] * sinf(4.0f*phi) * invSinPhi
                    )
                ;

            c *= derivOfPotential;
            scValues[2] *= derivOfPotential;
            float a11 = c * invLenSqrs[0] * scValues[0];
            float a22 = -invLenSqrs[1] * (2.0f*c0*scValues[2] - c*(scValues[0]+scValues[1]));
            float a33 = c*invLenSqrs[2]*scValues[1];
            float a12 = -invMagProds[0] * (c12Mags[0] * c * scValues[0] + c12Mags[1] * scValues[2]);
            float a13 = -invLens[0] * invLens[2] * scValues[2];
            float a23 = invMagProds[1] * (c12Mags[1]*c*scValues[1] + c12Mags[0]*scValues[2]);
            float3 sFloat3 = make_float3(
                    a12*directors[0].x + a22*directors[1].x + a23*directors[2].x
                    ,  a12*directors[0].y + a22*directors[1].y + a23*directors[2].y
                    ,  a12*directors[0].z + a22*directors[1].z + a23*directors[2].z
                    );
            //printf("ssomething valyes %f %f %f\n", sFloat3.x, sFloat3.y, sFloat3.z);
            //printf("comps %f %f %f %f %f %f\n", a12, directors[0].x,  a22, directors[1].x,  a23, directors[2].x);

            if (myIdxInDihedral <= 1) {
                float3 a11Dir1 = directors[0] * a11;
                float3 a12Dir2 = directors[1] * a12;
                float3 a13Dir3 = directors[2] * a13;
                myForce.x = a11Dir1.x + a12Dir2.x + a13Dir3.x;
                myForce.y = a11Dir1.y + a12Dir2.y + a13Dir3.y;
                myForce.z = a11Dir1.z + a12Dir2.z + a13Dir3.z;

                if (myIdxInDihedral == 1) {

                    myForce = -sFloat3 - myForce;
                }
            } else {
                float3 a13Dir1 = directors[0] * a13;
                float3 a23Dir2 = directors[1] * a23;
                float3 a33Dir3 = directors[2] * a33;
                myForce.x = a13Dir1.x + a23Dir2.x + a33Dir3.x;
                myForce.y = a13Dir1.y + a23Dir2.y + a33Dir3.y;
                myForce.z = a13Dir1.z + a23Dir2.z + a33Dir3.z;
                if (myIdxInDihedral == 2) {
                    myForce = sFloat3 - myForce;
                }
            }
            return myForce;


        }

};

#endif

            //    printf("deriv is %f\n", derivOfPotential);
            //    printf("coefs %f %f %f %f\n", dihedral.coefs[0],dihedral.coefs[1] , dihedral.coefs[2] , dihedral.coefs[3]);

/*

if (myIdxInDihedral <= 1) {
    float3 a11Dir1 = directors[0] * a11;
    float3 a12Dir2 = directors[1] * a12;
    float3 a13Dir3 = directors[2] * a13;
    myForce.x += a11Dir1.x + a12Dir2.x + a13Dir3.x;
    myForce.y += a11Dir1.y + a12Dir2.y + a13Dir3.y;
    myForce.z += a11Dir1.z + a12Dir2.z + a13Dir3.z;

    if (myIdxInDihedral == 1) {

        myForce = -sFloat3 - myForce;
        //      printf("dihedral idx 1 gets force %f %f %f\n", myForce.x, myForce.y, myForce.z);
    }
    //} else {
    //   printf("dihedral idx 0 gets force %f %f %f\n", myForce.x, myForce.y, myForce.z);
    //}
} else {
    float3 a13Dir1 = directors[0] * a13;
    float3 a23Dir2 = directors[1] * a23;
    float3 a33Dir3 = directors[2] * a33;
    myForce.x += a13Dir1.x + a23Dir2.x + a33Dir3.x;
    myForce.y += a13Dir1.y + a23Dir2.y + a33Dir3.y;
    myForce.z += a13Dir1.z + a23Dir2.z + a33Dir3.z;
    if (myIdxInDihedral == 2) {
        myForce = sFloat3 - myForce;
        //     printf("dihedral idx 2 gets force %f %f %f\n", myForce.x, myForce.y, myForce.z);
    }
}

            // } else {
            //     printf("dihedral idx 3 gets force %f %f %f\n", myForce.x, myForce.y, myForce.z);
            //}

            */
