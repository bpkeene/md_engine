#ifndef FIXDIHEDRALOPLS_H
#define FIXDIHEDRALOPLS_H
#include "FixPotentialMultiAtom.h"
#include "Dihedral.h"

void export_FixDihedralOPLS();
class FixDihedralOPLS : public FixPotentialMultiAtom<DihedralOPLS, DihedralOPLSGPU, 4> {
	public:
        FixDihedralOPLS(SHARED(State) state_, string handle);
		void compute(bool);
		//DataSet *eng;
		//DataSet *press;
        void createDihedral(Atom *, Atom *, Atom *, Atom *, double, double, double, double, int);
        //vector<pair<int, vector<int> > > neighborlistExclusions();
        string restartChunk(string format);


};

#endif
