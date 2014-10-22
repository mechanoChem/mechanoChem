#include <math.h> 
extern "C" {
#include "petiga.h"
}
#include "fields.h"
#include "petigaksp2.h"
#include <Sacado.hpp>

#define DIM 2
#define numVars 27
typedef Sacado::Fad::SFad<double,numVars> doubleAD;

struct AppCtx{ 
  IGA iga;
  double gamma, C, he, alpha, lambda, A4, A2, D, flux, dt, cbar;
  double A2_u, B2, B4, B6, C2, lambda_u;
  AppCtxKSP* appCtxKSP;
};

template <class T>
PetscErrorCode Function(IGAPoint p,PetscReal dt2,
				PetscReal shift,const PetscScalar *V,
				PetscReal t,const T * U,
				PetscReal t0,const PetscScalar * U0,
				T *R,void *ctx)
{
  AppCtx *user = (AppCtx *) ctx;
  unsigned int dim=p->dim;
  PetscInt nen, dof;
  IGAPointGetSizes(p,0,&nen,&dof);
  PetscReal *n = p->normal;
 //concentration field variable
  T c, cx[DIM], cxx[DIM][DIM]; PetscReal c0;
  computeField<T,DIM,DIM+1>(SCALAR,0,p,U,&c,&cx[0],&cxx[0][0]);
  computeField<PetscReal,DIM,DIM+1>(SCALAR,0,p,U0,&c0);
  
  //displacement field variable
  T u[DIM], ux[DIM][DIM], uxx[DIM][DIM][DIM];
  computeField<T,DIM,DIM+1>(VECTOR,1,p,&U[0],&u[0],&ux[0][0],&uxx[0][0][0]);

  //chemical potential
  T mu =  user->A4*c*c*c + user->A2*(user->alpha-1)*c;
  T dmu =  3*user->A4*c*c + user->A2*(user->alpha-1);
  T M=user->D;

  //Compute F
  T F[dim][dim], dF[dim][dim][dim];
  for (unsigned int i=0; i<dim; i++) {
    for (unsigned int j=0; j<dim; j++) {
      F[i][j]=(i==j)+ux[i][j];
      for (unsigned int k=0; k<dim; k++) {
	dF[i][j][k]=uxx[i][j][k];
      }
    }
  }
  //Compute strain metric, E  (E=0.5*(F^T*F-I))
  T E[dim][dim];
  for (unsigned int i=0; i<dim; i++){
    for (unsigned int j=0; j<dim; j++){
      E[i][j] = -0.5*(i==j);
      for (unsigned int k=0; k<dim; k++){
	E[i][j] += 0.5*F[k][i]*F[k][j];
      }
    }
  }
  //new strain metrics
  T e1=(E[0][0]+E[1][1]);
  T e2=(E[0][0]-E[1][1]);
  T e3=E[0][1];
  T e2_1=0.0, e2_2=0.0;
  for (unsigned int A=0; A<dim; ++A){
    e2_1+=(F[A][0]*dF[A][0][0]-F[A][1]*dF[A][1][0]);
    e2_2+=(F[A][0]*dF[A][0][1]-F[A][1]*dF[A][1][1]);
  }
  //material constants
  PetscReal A2=user->A2_u, B2=user->B2, B4=user->B4, B6=user->B6, C2=user->C2, lambda=user->lambda_u;
  //compute P and Beta
  T P[dim][dim], Beta[dim][dim][dim];
  for (unsigned int i=0; i<dim; ++i){
    for (unsigned int J=0; J<dim; ++J){
      T e1_FiJ=(F[i][0]*(0==J)+F[i][1]*(1==J));
      T e2_FiJ=(F[i][0]*(0==J)-F[i][1]*(1==J));
      T e3_FiJ=(F[i][1]*(0==J)+F[i][0]*(1==J))/2.0;
      P[i][J]=(2*A2*e1)*e1_FiJ +					\
	(2*B2*(-c)*e2 + 4*B4*e2*e2*e2 + 6*B6*e2*e2*e2*e2*e2)*e2_FiJ + \
	(2*C2*e3)*e3_FiJ;
      
      //gradient terms
      for (unsigned int K=0; K<dim; ++K){
	T e2_1_FiJK=(F[i][0]*(0==J)-F[i][1]*(1==J))*(0==K);
	T e2_2_FiJK=(F[i][0]*(0==J)-F[i][1]*(1==J))*(1==K);
	Beta[i][J][K]= 2.0*lambda* (e2_1*e2_1_FiJK + e2_2*e2_2_FiJK);
      }
    }
  }

  
  /* //get shape function values */
  double (*N) = (double (*)) p->shape[0];
  double (*Nx)[dim] = (double (*)[dim]) p->shape[1];
  double (*Nxx)[dim][dim] = (double (*)[dim][dim]) p->shape[2];

  //Compute Residual
  bool surfaceFlag=p->atboundary;
  T (*Ra)[dof] = (T (*)[dof])R;
  for (unsigned int a=0; a<(unsigned int)nen; a++) {
    double N1[dim], N2[dim][dim];
    for (unsigned int i=0; i<dim; i++){
      N1[i]=Nx[a][i];
      for (unsigned int j=0; j<dim; j++){
	N2[i][j]=Nxx[a][i][j];
      }
    }
    
    //Chemistry
    T laplace_c=0;
    for (unsigned int i=0; i<dim; i++) laplace_c+=cxx[i][i];
    T Rc=0.0;
    if (!surfaceFlag){
      // Na * c_t
      Rc += N[a] * (c-c0)*(1.0/user->dt);
      // grad(Na) . (M*dmu) grad(C)
      T t1 = M*dmu;
      double laplace_N=0.0;
      for (unsigned int i=0; i<dim; i++){
	Rc += N1[i]*t1*cx[i];
	laplace_N += N2[i][i];
      }
      // (lambda/2.0) * del2(Na) * M * del2(c)
      Rc += (user->lambda/2.0)*laplace_N*M*laplace_c;
    }
    else{
      // -grad(Na) . (M*del2(c)) n
      T t1 = M*laplace_c;
      double laplace_N=0.0;
      for (unsigned int i=0; i<dim; i++){
	Rc += -N1[i]*t1*n[i];
	laplace_N += N2[i][i];
      }
      // -(gamma*del2(Na)*M)*grad(C).n
      T t2 = user->gamma*laplace_N*M;
      for (unsigned int i=0; i<dim; i++){
	Rc += -t2*cx[i]*n[i];
      }
      // (C/he)*(grad(Na).n)*M*(grad(C).n)
      double t3=0.0;
      T t4 = (user->C/user->he)*M;
      T t5=0.0;
      for (unsigned int i=0; i<dim; i++){
	t3 += N1[i]*n[i];
	t5 += cx[i]*n[i];
      }
      Rc += t3*t4*t5;
      Rc *= (user->lambda/2.0);
      //flux term
      // Na*J
      double fluxValue=user->flux;
      if (n[1]==0) fluxValue*=0.0;
      Rc += -N[a]*fluxValue;
    }
    Ra[a][0] = Rc;

    //Mechanics
    if (!surfaceFlag) {
      for (unsigned int i=0; i<dim; i++){
	T Ru_i=0.0;
	for (unsigned int j=0; j<dim; j++){
	  //grad(Na)*P
	  Ru_i += N1[j]*P[i][j];
	  for (unsigned int k=0; k<dim; k++){
	    Ru_i += N2[j][k]*Beta[i][j][k];
	  }
	}
	Ra[a][i+1] = Ru_i;
      }
    }
  }
  return 0;
}


#undef  __FUNCT__
#define __FUNCT__ "Residual"
PetscErrorCode Residual(IGAPoint p,PetscReal dt,
                        PetscReal shift,const PetscScalar *V,
                        PetscReal t,const PetscScalar *U,
                        PetscReal t0,const PetscScalar *U0, 
			PetscScalar *R,void *ctx)
{
  Function(p, dt, shift, V, t, U, t0, U0, R, ctx);
  return 0;
}

#undef  __FUNCT__
#define __FUNCT__ "Jacobian"
PetscErrorCode Jacobian(IGAPoint p,PetscReal dt,
				PetscReal shift,const PetscScalar *V,
				PetscReal t,const PetscScalar *U,
				PetscReal t0,const PetscScalar *U0,
				PetscScalar *K,void *ctx)
{
  AppCtx *user = (AppCtx *)ctx;
  const PetscInt nen=p->nen, dof=DIM+1;
  const PetscReal (*U2)[DIM+1] = (PetscReal (*)[DIM+1])U;
  if (dof*nen!=numVars) {
    PetscPrintf(PETSC_COMM_WORLD,"\ndof*nen!=numVars.... Set numVars = %u\n",dof*nen); exit(-1);
  }
  std::vector<doubleAD> U_AD(nen*dof);
  for(int i=0; i<nen*dof; i++){
    U_AD[i]=U[i];
    U_AD[i].diff(i, dof*nen);
  } 
  std::vector<doubleAD> R(nen*dof);
  Function<doubleAD> (p, dt, shift, V, t, &U_AD[0], t0, U0, &R[0], ctx);
  for(int n1=0; n1<nen; n1++){
    for(int d1=0; d1<dof; d1++){
      for(int n2=0; n2<nen; n2++){
	for(int d2=0; d2<dof; d2++){
      	  K[n1*dof*nen*dof + d1*nen*dof + n2*dof + d2] = R[n1*dof+d1].dx(n2*dof+d2);
	}
      }
    }				
  }
  return 0;    
}

PetscErrorCode E22System(IGAPoint p, PetscScalar *K, PetscScalar *R, void *ctx)
{
  AppCtxKSP *user = (AppCtxKSP *)ctx;
  PetscInt nen, dof, dim=p->dim;
  IGAPointGetSizes(p,0,&nen,&dof);
  //displacement field variables
  double u[dim], ux[dim][dim];
  computeField<PetscReal,DIM,DIM+1>(VECTOR,1,p,user->localU0,&u[0],&ux[0][0]);

  //Compute F
  PetscReal F[dim][dim];
  for (PetscInt i=0; i<dim; i++) {
    for (PetscInt j=0; j<dim; j++) {
      F[i][j]=(i==j)+ux[i][j];
    }
  }
  
  //Compute strain metric, E  (E=0.5*(F^T*F-I))
  PetscReal E[dim][dim];
  for (PetscInt i=0; i<dim; i++){
    for (PetscInt j=0; j<dim; j++){
      E[i][j] = -0.5*(i==j);
      for (PetscInt k=0; k<dim; k++){
	E[i][j] += 0.5*F[k][i]*F[k][j];
      }
    }
  }
  
  PetscReal e1=(E[0][0]+E[1][1]);
  PetscReal e2=(E[0][0]-E[1][1]);
  PetscReal e3=E[0][1];

  //store L2 projection residual
  const PetscReal (*N) = (PetscReal (*)) p->shape[0];;  
  for(int n1=0; n1<nen; n1++){
    for(int d1=0; d1<dof; d1++){
      PetscReal val=0.0;
      switch (d1) {
      case 0:
	val=e2; break;
      case 1:
	val=e3; break;
      case 2: 
	val=e1; break;
      } 
      R[n1*dof+d1] = N[n1]*val;
      for(int n2=0; n2<nen; n2++){
	for(int d2=0; d2<dof; d2++){
	  PetscReal val2=0.0;
	  if (d1==d2) {val2 = N[n1] * N[n2];}
	  K[n1*dof*nen*dof + d1*nen*dof + n2*dof + d2] =val2;
	}
      }
    }
  }
  return 0;
}

PetscErrorCode ProjectSolution(IGA iga, PetscInt step, AppCtxKSP *user)
{	
  PetscErrorCode ierr;
  PetscFunctionBegin;
  //Setup linear system for L2 Projection
  Mat A;
  Vec x,b;
  ierr = IGACreateMat(iga,&A);CHKERRQ(ierr);
  ierr = IGACreateVec(iga,&x);CHKERRQ(ierr);
  ierr = IGACreateVec(iga,&b);CHKERRQ(ierr);
  ierr = IGASetFormSystem(iga,E22System,user);CHKERRQ(ierr);
  ierr = IGAComputeSystem2(iga,A,b);CHKERRQ(ierr);

  //Solver
  KSP ksp;
  ierr = IGACreateKSP(iga,&ksp);CHKERRQ(ierr);
  //ierr = KSPSetOperators(ksp,A,A,SAME_NONZERO_PATTERN);CHKERRQ(ierr);
  ierr = KSPSetOperators(ksp,A,A);CHKERRQ(ierr);
  ierr = KSPSetFromOptions(ksp);CHKERRQ(ierr);
  ierr = KSPSolve(ksp,b,x);CHKERRQ(ierr);
  //write solution
  char filename[256];
  sprintf(filename,"./outE%d.dat",step);
  ierr = IGAWriteVec(iga,x,filename);CHKERRQ(ierr);
  ierr = KSPDestroy(&ksp);CHKERRQ(ierr);
  ierr = MatDestroy(&A);CHKERRQ(ierr);
  ierr = VecDestroy(&x);CHKERRQ(ierr);
  ierr = VecDestroy(&b);CHKERRQ(ierr);
  PetscFunctionReturn(0); 
}

typedef struct {
  PetscReal c, ux, uy;
} Field;

template <int dim>
PetscErrorCode FormInitialCondition(IGA iga, Vec U, AppCtx *user)
{	
  PetscErrorCode ierr;
  PetscFunctionBegin;
  std::srand(5);
  DM da;
  ierr = IGACreateNodeDM(iga,1+DIM,&da);CHKERRQ(ierr);
  Field **u;
  ierr = DMDAVecGetArray(da,U,&u);CHKERRQ(ierr);
  DMDALocalInfo info;
  ierr = DMDAGetLocalInfo(da,&info);CHKERRQ(ierr);

  PetscInt i,j;
  for(i=info.xs;i<info.xs+info.xm;i++){
    for(j=info.ys;j<info.ys+info.ym;j++){
      u[j][i].c = user->cbar + 0.01*(0.5 - (double)(std::rand() % 100 )/100.0);
      u[j][i].ux=0.0;
      u[j][i].uy=0.0;
    }
  }
  ierr = DMDAVecRestoreArray(da,U,&u);CHKERRQ(ierr); 
  ierr = DMDestroy(&da);;CHKERRQ(ierr); 
  PetscFunctionReturn(0); 
}

#undef __FUNCT__
#define __FUNCT__ "OutputMonitor"
PetscErrorCode OutputMonitor(TS ts,PetscInt it_number,PetscReal c_time,Vec U,void *mctx)
{
  PetscFunctionBegin;
  PetscErrorCode ierr;
  AppCtx *user = (AppCtx *)mctx;
  char           filename[256];
  sprintf(filename,"./outU%d.dat",it_number);
  ierr = IGAWriteVec(user->iga,U,filename);CHKERRQ(ierr);
  ProjectSolution(user->iga, it_number, user->appCtxKSP);
  PetscFunctionReturn(0);
}


int main(int argc, char *argv[]) {
  PetscErrorCode  ierr;
  ierr = PetscInitialize(&argc,&argv,0,0);CHKERRQ(ierr);
  double startTime = MPI_Wtime();
 
  /* Define simulation specific parameters */
  AppCtx user; AppCtxKSP userKSP;
  user.cbar  	= -0.9;   /* average concentration */
  user.A4 	= 7.3e-3;
  user.A2   	= 6.6e-3;
  user.flux	= 1000.0;
  user.lambda   = 1.0e-6;
  user.D	= 100000.0;
  user.alpha	= 0.0;
  user.dt       = 1.0e-7; 
  user.gamma    = 1;
  user.C        = 5;
  double relFac=1.0; //10.0;
  double scaleMech=1.0e2;
  user.A2_u= pow(scaleMech,2)*relFac*0.05/2;
  user.B2= pow(scaleMech,2)*relFac*0.0025/2;
  user.B4= pow(scaleMech,4)*relFac*(-0.0031/4);
  user.B6= pow(scaleMech,6)*relFac*0.3/6;
  user.C2= pow(scaleMech,2)*relFac*0.05/2;
  user.lambda_u = relFac*(user.lambda);
  
  /* Set discretization options */
  PetscInt nsteps = 100000;
  PetscInt N=200, p=2, C=PETSC_DECIDE, resStep=0;
  PetscBool output = PETSC_FALSE; 
  PetscBool monitor = PETSC_FALSE; 
  PetscReal lambdaScale=10.0, fluxScale=4.0, DScale=1.0;
  char filePrefix[PETSC_MAX_PATH_LEN] = {0};
  ierr = PetscOptionsBegin(PETSC_COMM_WORLD,"","CahnHilliard2D Options","IGA");CHKERRQ(ierr);
  ierr = PetscOptionsInt("-N","number of elements (along one dimension)",__FILE__,N,&N,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsInt("-p","polynomial order",__FILE__,p,&p,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsInt("-C","global continuity order",__FILE__,C,&C,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsString("-file_prefix","File Prefix",__FILE__,filePrefix,filePrefix,sizeof(filePrefix),PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsBool("-ch_output","Enable output files",__FILE__,output,&output,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsBool("-ch_monitor","Compute and show statistics of solution",__FILE__,monitor,&monitor,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsInt("-nsteps","Number of load steps to take",__FILE__,nsteps,&nsteps,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal("-dt","time step",__FILE__,user.dt,&user.dt,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal("-lambda","lambda",__FILE__,lambdaScale,&lambdaScale,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal("-flux","flux",__FILE__,fluxScale,&fluxScale,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal("-D","Diffusivity",__FILE__,DScale,&DScale,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsInt("-res_step","Restart Step",__FILE__,resStep,&resStep,PETSC_NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEnd();CHKERRQ(ierr);
  if (C == PETSC_DECIDE) C = p-1;
  //
  user.flux*=fluxScale;
  user.D*=DScale;
  user.lambda_u*=lambdaScale;

  PetscPrintf(PETSC_COMM_WORLD,"\nLambda_u value is: %8.2e\n",user.lambda_u);
  user.he=1.0/N;
  //
  if (p < 2 || C < 0) /* Problem requires a p>=2 C1 basis */
    SETERRQ(PETSC_COMM_WORLD,PETSC_ERR_ARG_OUTOFRANGE,"Problem requires minimum of p = 2");
  if (p <= C)         /* Check C < p */
    SETERRQ(PETSC_COMM_WORLD,PETSC_ERR_ARG_OUTOFRANGE,"Discretization inconsistent: polynomial order must be greater than degree of continuity");
  
  IGA iga;
  ierr = IGACreate(PETSC_COMM_WORLD,&iga);CHKERRQ(ierr);
  ierr = IGASetDim(iga,DIM);CHKERRQ(ierr);
  ierr = IGASetDof(iga,1+DIM);CHKERRQ(ierr);

  IGAAxis axis0;
  ierr = IGAGetAxis(iga,0,&axis0);CHKERRQ(ierr);
  ierr = IGAAxisSetDegree(axis0,p);CHKERRQ(ierr);
  ierr = IGAAxisInitUniform(axis0,N,0.0,1.0,C);CHKERRQ(ierr);
  IGAAxis axis1;
  ierr = IGAGetAxis(iga,1,&axis1);CHKERRQ(ierr);
  ierr = IGAAxisCopy(axis0,axis1);CHKERRQ(ierr);

  ierr = IGASetFromOptions(iga);CHKERRQ(ierr);
  ierr = IGASetUp(iga);CHKERRQ(ierr);
  user.iga = iga;
  if (resStep==0){
    char meshfilename[256];
    sprintf(meshfilename, "mesh.dat");
    PetscPrintf(PETSC_COMM_WORLD,"\nWriting mesh file: %s\n", meshfilename);
    ierr = IGAWrite(iga, meshfilename);CHKERRQ(ierr);
  }
  
  Vec U,U0;
  ierr = IGACreateVec(iga,&U);CHKERRQ(ierr);
  ierr = IGACreateVec(iga,&U0);CHKERRQ(ierr);
  if (resStep>0){
    MPI_Comm comm;
    PetscViewer viewer;
    char restartfilename[256];
    ierr = PetscObjectGetComm((PetscObject)U0,&comm);CHKERRQ(ierr);
    sprintf(restartfilename,"res%s-%d.dat",filePrefix,resStep);
    ierr = PetscViewerBinaryOpen(comm,restartfilename,FILE_MODE_READ,&viewer);CHKERRQ(ierr);
    ierr = VecLoad(U0,viewer);CHKERRQ(ierr);
    ierr = PetscViewerDestroy(&viewer);
    PetscPrintf(PETSC_COMM_WORLD,"\nReading solution from restart file: %s\n",restartfilename);
  }
  else{
    ierr = FormInitialCondition<DIM>(iga, U0, &user); 
  }
  ierr = VecCopy(U0, U);CHKERRQ(ierr);
  user.appCtxKSP=&userKSP;
  userKSP.U0=&U;

  //
  IGAForm form;
  ierr = IGAGetForm(iga,&form);CHKERRQ(ierr);
  ierr = IGAFormSetBoundaryForm (form,0,0,PETSC_TRUE);CHKERRQ(ierr);
  ierr = IGAFormSetBoundaryForm (form,0,1,PETSC_TRUE);CHKERRQ(ierr);
  ierr = IGAFormSetBoundaryForm (form,1,0,PETSC_TRUE);CHKERRQ(ierr);
  ierr = IGAFormSetBoundaryForm (form,1,1,PETSC_TRUE);CHKERRQ(ierr);

  //Dirichlet BC
  ierr = IGASetBoundaryValue(iga,0,0,1,0.0);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,0,0,2,0.0);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,1,0,1,0.0);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,1,0,2,0.0);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,0,1,1,0.0001);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,0,1,2,0.0);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,1,1,1,0.0);CHKERRQ(ierr);
  ierr = IGASetBoundaryValue(iga,1,1,2,0.0);CHKERRQ(ierr);

  ierr = IGASetFormIEFunction(iga,Residual,&user);CHKERRQ(ierr);
  ierr = IGASetFormIEJacobian(iga,Jacobian,&user);CHKERRQ(ierr);

  TS ts;
  ierr = IGACreateTS(iga,&ts);CHKERRQ(ierr);
  ierr = TSSetType(ts,TSBEULER);CHKERRQ(ierr);
  ierr = TSSetDuration(ts,100000,1.0);CHKERRQ(ierr);
  ierr = TSSetTime(ts,0.0);CHKERRQ(ierr);
  ierr = TSSetTimeStep(ts,user.dt);CHKERRQ(ierr);
  ierr = TSMonitorSet(ts,OutputMonitor,&user,NULL);CHKERRQ(ierr);
  
  //SNES snes;
  //TSGetSNES(ts,&snes);
  //SNESSetConvergenceTest(snes,SNESConverged_Interactive,(void*)&user,NULL); 
  //SNESLineSearch ls;
  //SNESGetLineSearch(snes,&ls);
  //SNESLineSearchSetType(ls,SNESLINESEARCHBT);
  //ierr = SNESSetFromOptions(snes);CHKERRQ(ierr);
  // SNESLineSearchView(ls,NULL);
  
  ierr = TSSetFromOptions(ts);CHKERRQ(ierr);
#if PETSC_VERSION_LE(3,3,0)
  ierr = TSSolve(ts,U,NULL);CHKERRQ(ierr);
#else
  ierr = TSSolve(ts,U);CHKERRQ(ierr);
#endif
  //
  ierr = VecDestroy(&U);CHKERRQ(ierr);
  ierr = VecDestroy(&U0);CHKERRQ(ierr);
  ierr = TSDestroy(&ts);CHKERRQ(ierr);
  ierr = IGADestroy(&iga);CHKERRQ(ierr);
  ierr = PetscFinalize();CHKERRQ(ierr);
  return 0;
}
