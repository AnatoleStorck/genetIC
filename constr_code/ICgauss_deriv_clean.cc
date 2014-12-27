#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sstream>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <complex>

#include <gsl/gsl_rng.h> //link -lgsl and -lgslcblas at the very end
#include <gsl/gsl_randist.h> //for the gaussian (and other) distributions
#include <gsl/gsl_errno.h>
#include <gsl/gsl_spline.h>

#include "constrainer.h"

using namespace std;

#ifdef HAVE_HDF5
#include "HDF_IO.hh"
#endif

#ifndef DOUBLEPRECISION     /* default is single-precision */
typedef float  MyFloat;
typedef float  MyDouble;
#include <srfftw.h>
#ifdef HAVE_HDF5
hid_t hdf_float = H5Tcopy (H5T_NATIVE_FLOAT);
hid_t hdf_double = H5Tcopy (H5T_NATIVE_FLOAT);
#endif
//#else
//#if (DOUBLEPRECISION == 2)   /* mixed precision, do we want to implement this at all? */
//typedef float   MyFloat;
//typedef double  MyDouble;
//hid_t hdf_float = H5Tcopy (H5T_NATIVE_FLOAT);
//hid_t hdf_double = H5Tcopy (H5T_NATIVE_DOUBLE);
#else                        /* everything double-precision */
typedef double  MyFloat;
typedef double  MyDouble;
#ifdef FFTW_TYPE_PREFIX
#include <drfftw.h>
#else
#include <rfftw.h>
#endif
#ifdef HAVE_HDF5
hid_t hdf_float = H5Tcopy (H5T_NATIVE_DOUBLE);
hid_t hdf_double = H5Tcopy (H5T_NATIVE_DOUBLE);
#endif
#endif
// #endif

#ifdef OUTPUT_IN_DOUBLEPRECISION //currently not used, output is same as MyFloat
typedef double MyOutputFloat;
#else
typedef float MyOutputFloat;
#endif

//stupid workaround because apparently pow() in C++ does not accept floats, only doubles...
MyFloat powf(MyFloat base, MyFloat exp)
{

  double result;
  result=pow(double(base), double(exp));

  return MyFloat(result);

}

size_t my_fwrite(void *ptr, size_t size, size_t nmemb, FILE * stream) //stolen from Gadget
{
  size_t nwritten;

  if((nwritten = fwrite(ptr, size, nmemb, stream)) != nmemb)
    {
      printf("I/O error (fwrite) has occured.\n");
      fflush(stdout);
    }
  return nwritten;
}

struct io_header_2 //header for gadget2
{
  int      npart[6];
  double   mass[6];
  double   time;
  double   redshift;
  int      flag_sfr;
  int      flag_feedback;
  int      npartTotal[6]; /*!< npart[1] gives the total number of particles in the run. If this number exceeds 2^32, the npartTotal[2] stores the result of a division of the particle number by 2^32, while npartTotal[1] holds the remainder. */
  int      flag_cooling;
  int      num_files;
  double   BoxSize;
  double   Omega0;
  double   OmegaLambda;
  double   HubbleParam;
  char     fill[256- 6*4- 6*8- 2*8- 2*4- 6*4- 2*4 - 4*8];  /* fills to 256 Bytes */
} header2;


struct io_header_3 //header for gadget3
{
  int npart[6];			/*!< number of particles of each type in this file */
  double mass[6];		/*!< mass of particles of each type. If 0, then the masses are explicitly
				   stored in the mass-block of the snapshot file, otherwise they are omitted */
  double time;			/*!< time of snapshot file */
  double redshift;		/*!< redshift of snapshot file */
  int flag_sfr;			/*!< flags whether the simulation was including star formation */
  int flag_feedback;		/*!< flags whether feedback was included (obsolete) */
  unsigned int npartTotal[6];	/*!< total number of particles of each type in this snapshot. This can be
				   different from npart if one is dealing with a multi-file snapshot. */
  int flag_cooling;		/*!< flags whether cooling was included  */
  int num_files;		/*!< number of files in multi-file snapshot */
  double BoxSize;		/*!< box-size of simulation in case periodic boundaries were used */
  double Omega0;		/*!< matter density in units of critical density */
  double OmegaLambda;		/*!< cosmological constant parameter */
  double HubbleParam;		/*!< Hubble parameter in units of 100 km/sec/Mpc */
  int flag_stellarage;		/*!< flags whether the file contains formation times of star particles */
  int flag_metals;		/*!< flags whether the file contains metallicity values for gas and star
				   particles */
  unsigned int npartTotalHighWord[6];	/*!< High word of the total number of particles of each type (see header2)*/
  int flag_entropy_instead_u;	/*!< flags that IC-file contains entropy instead of u */
  int flag_doubleprecision;	/*!< flags that snapshot contains double-precision instead of single precision */

  int flag_ic_info;             /*!< flag to inform whether IC files are generated with ordinary Zeldovich approximation,
                                     or whether they ocontains 2nd order lagrangian perturbation theory initial conditions.
                                     For snapshots files, the value informs whether the simulation was evolved from
                                     Zeldoch or 2lpt ICs. Encoding is as follows:
                                        FLAG_ZELDOVICH_ICS     (1)   - IC file based on Zeldovich
                                        FLAG_SECOND_ORDER_ICS  (2)   - Special IC-file containing 2lpt masses
                                        FLAG_EVOLVED_ZELDOVICH (3)   - snapshot evolved from Zeldovich ICs
                                        FLAG_EVOLVED_2LPT      (4)   - snapshot evolved from 2lpt ICs
                                        FLAG_NORMALICS_2LPT    (5)   - standard gadget file format with 2lpt ICs
                                     All other values, including 0 are interpreted as "don't know" for backwards compatability.
                                 */
  float lpt_scalingfactor;      /*!< scaling factor for 2lpt initial conditions */

  char fill[48];		/*!< fills to 256 Bytes */

} header3;				/*!< holds header for snapshot files */

#ifdef HAVE_HDF5
void SaveHDF(const char* Filename, int n, io_header_3 header, MyFloat *p_Data1,  MyFloat *p_Data2, MyFloat *p_Data3, MyFloat *v_Data1,  MyFloat *v_Data2, MyFloat *v_Data3, const char *name1, const char *name2, const char *name3) //saves 3 arrays as datasets "name1", "name2" and "name3" in one HDF5 file
{
  hid_t       file_id, gidh, gid, dset_id,ndset_id,iddset_id, hdset_id,mt_id;   /* file and dataset identifiers */
  hid_t       nfilespace,dataspace_id, memspace, hboxfs,mt,idfilespace,IDmemspace,iddataspace_id;/* file and memory dataspace identifiers */
  hsize_t ncount[2],ncount2[2], noffset[2], h[1], stride[2],  idcount[2],idoffset[2],idstride[2],mta[1],idblock[2];
  h[0]=1;
  int m_nGrid[3]={n,n,n};
  mta[0]=6;

  file_id = H5Fcreate( Filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT );
  gidh=H5Gcreate(file_id, "Header", 100 ); //100 gives max number of arguments (?)

   MyFloat *box=new MyFloat[1];
   MyFloat *mtt=new MyFloat[6];
   int *boxi=new int[1];
   mtt[0]=header.mass[0];
   mtt[1]=header.mass[1];
   mtt[2]=header.mass[2];
   mtt[3]=header.mass[3];
   mtt[4]=header.mass[4];
   mtt[5]=header.mass[5];
   mt = H5Screate_simple( 1, mta, NULL );

   mt_id = H5Acreate( gidh, "MassTable", hdf_float, mt, H5P_DEFAULT );
   H5Awrite( mt_id, hdf_float, mtt);
   H5Aclose(mt_id);

   hboxfs = H5Screate_simple( 1, h, NULL );
   box[0]=header.Omega0;
   hdset_id = H5Acreate( gidh, "OM", hdf_float, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, hdf_float, box);
   H5Aclose(hdset_id);

   box[0]=header.OmegaLambda;
   hdset_id = H5Acreate( gidh, "OLambda", hdf_float, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, hdf_float, box);
   H5Aclose(hdset_id);

   box[0]=header.BoxSize;
   hdset_id = H5Acreate( gidh, "BoxSize", hdf_float, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, hdf_float, box);
   H5Aclose(hdset_id);

   box[0]=header.redshift;
   hdset_id = H5Acreate( gidh, "Redshift", hdf_float, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, hdf_float, box);
   H5Aclose(hdset_id);

   box[0]=header.time;
   hdset_id = H5Acreate( gidh, "Time", hdf_float, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, hdf_float, box);
   H5Aclose(hdset_id);

   //box[0]=0.; //fix: read as input
   //hdset_id = H5Acreate( gidh, "fnl", hdf_float, hboxfs, H5P_DEFAULT );
   //H5Awrite( hdset_id, hdf_float, box);
   //H5Aclose(hdset_id);

   box[0]=0.817;//fix: read as input
   hdset_id = H5Acreate( gidh, "sigma8", hdf_float, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, hdf_float, box);
   H5Aclose(hdset_id);

   long *boxu=(long*)calloc(6,sizeof(long));
   boxu[1]=(long)(m_nGrid[0]*m_nGrid[1]*m_nGrid[2]);
   hdset_id = H5Acreate( gidh, "NumPart_Total", H5T_NATIVE_UINT, mt, H5P_DEFAULT );
   H5Awrite( hdset_id, H5T_NATIVE_UINT, boxu);
   H5Aclose(hdset_id);

//    hdset_id = H5Acreate( gidh, "NumPart_ThisFile", H5T_NATIVE_UINT, mt, H5P_DEFAULT );
//    H5Awrite( hdset_id, H5T_NATIVE_UINT, boxu);
//    H5Aclose(hdset_id);

   boxu[1]=header.npartTotalHighWord[1];
   hdset_id = H5Acreate( gidh, "NumPart_Total_HighWord", H5T_NATIVE_UINT, mt, H5P_DEFAULT );
   H5Awrite( hdset_id, H5T_NATIVE_UINT, boxu);
   H5Aclose(hdset_id);

   boxi[0]=header.flag_ic_info;
   hdset_id = H5Acreate( gidh, "Flag_IC_Info", H5T_NATIVE_INT, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, H5T_NATIVE_INT, boxi);
   H5Aclose(hdset_id);

   boxi[0]=header.flag_doubleprecision;
   hdset_id = H5Acreate( gidh, "flag_doubleprecision", H5T_NATIVE_INT, hboxfs, H5P_DEFAULT );
   H5Awrite( hdset_id, H5T_NATIVE_INT, boxi);
   H5Aclose(hdset_id);

//    boxi[0]=header.num_files;
//    hdset_id = H5Acreate( gidh, "NumFilesPerSnapshot", H5T_NATIVE_INT, hboxfs, H5P_DEFAULT );
//    H5Awrite( hdset_id, H5T_NATIVE_INT, boxi);
//    H5Aclose(hdset_id);


   H5Sclose(mt);
   H5Sclose(hboxfs);

    //close "header" group
       H5Gclose(gidh);

  gid=H5Gcreate(file_id, "PartType1", 100 );
  idcount[0]=m_nGrid[0]*m_nGrid[1]*m_nGrid[2];
  idcount[1]=1;
  idfilespace = H5Screate_simple( 2, idcount, NULL );
  iddset_id = H5Dcreate( gid, name3, H5T_NATIVE_LONG, idfilespace, H5P_DEFAULT );
  H5Sclose(idfilespace);

  IDmemspace = H5Screate_simple( 2, idcount, NULL );

  idoffset[0]=0;
  idoffset[1]=0;
  idstride[0]=1;
  idstride[1]=1;
  idblock[0]=1;
  idblock[1]=1;

  long i;
  long* ID=(long*)calloc(n*n*n,sizeof(long));
  for (i=0; i< (long)(n*n*n); i++){ID[i]=i;}

  iddataspace_id= H5Dget_space(iddset_id);
  H5Sselect_hyperslab(iddataspace_id, H5S_SELECT_SET, idoffset, idstride, idcount, idblock);
  H5Dwrite( iddset_id, H5T_NATIVE_LONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, ID );

  free(ID);

  H5Sclose(iddataspace_id);
  H5Sclose(IDmemspace);
  H5Dclose(iddset_id);

  ncount[0]=m_nGrid[0]*m_nGrid[1]*m_nGrid[2]; //total number of particles
  ncount[1]=3; //3 components of position vector
  nfilespace = H5Screate_simple( 2, ncount, NULL ); //works! for (n**3, (x1,x2,x3) ) array (part pos./vel.)
  dset_id = H5Dcreate( gid, name1, hdf_float, nfilespace, H5P_DEFAULT );
  ndset_id = H5Dcreate( gid, name2, hdf_float, nfilespace, H5P_DEFAULT );
  H5Sclose(nfilespace);

  noffset[0] = 0;
  noffset[1] = 0;

  ncount2[0]=m_nGrid[0]*m_nGrid[1]*m_nGrid[2];
  ncount2[1]=1;

  memspace = H5Screate_simple( 2, ncount2, NULL );

  dataspace_id= H5Dget_space(dset_id);

  stride[0]=1;
  stride[1]=1;

  H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, noffset, stride, ncount2, NULL);
    H5Dwrite( dset_id, hdf_float, memspace, dataspace_id, H5P_DEFAULT, p_Data1 );

    noffset[0] = 0;
    noffset[1] = 1;

    H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, noffset, stride, ncount2, NULL);
    H5Dwrite( dset_id, hdf_float, memspace, dataspace_id, H5P_DEFAULT, p_Data2 );

    noffset[0] = 0;
    noffset[1] = 2;

    H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, noffset, stride, ncount2, NULL);
    H5Dwrite( dset_id, hdf_float, memspace, dataspace_id, H5P_DEFAULT, p_Data3 );

    noffset[0] = 0;
    noffset[1] = 0;
   H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, noffset, NULL, ncount2, NULL);
   H5Dwrite( ndset_id, hdf_float, memspace, dataspace_id, H5P_DEFAULT, v_Data1 );

   noffset[0] = 0;
    noffset[1] = 1;
   H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, noffset, NULL, ncount2, NULL);
   H5Dwrite( ndset_id, hdf_float, memspace, dataspace_id, H5P_DEFAULT, v_Data2 );

   noffset[0] = 0;
    noffset[1] = 2;
   H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, noffset, NULL, ncount2, NULL);
   H5Dwrite( ndset_id, hdf_float, memspace, dataspace_id, H5P_DEFAULT, v_Data3 );

  H5Sclose(memspace);
  H5Dclose(dset_id);
  H5Dclose(ndset_id);
  H5Sclose(dataspace_id);

  //close "particle" group
    H5Gclose(gid);

      //close overall file
         H5Fclose(file_id);

    free(box);
    free(boxi);
    free(boxu);
    free(mtt);

}

int save_phases(complex<MyFloat> *phk, MyFloat* ph, complex<MyFloat> *delta, int n, const char *name){

      MyFloat *helper=(MyFloat*)calloc(n*n*n, sizeof(MyFloat));
      int i;
      for(i=0;i<n*n*n;i++){helper[i]=delta[i].real();}

	hid_t     idfilespace,  file_id, gid, iddset_id,IDmemspace,iddataspace_id ;
	hsize_t idcount[2];
	file_id = H5Fcreate( name, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT );
	gid=H5Gcreate(file_id, "Potential", 100 );
	idcount[0]=n*n*n;
	idcount[1]=2;
	idfilespace = H5Screate_simple( 2, idcount, NULL );
	iddset_id = H5Dcreate( gid, "Psi_k", hdf_float, idfilespace, H5P_DEFAULT );
	H5Sclose(idfilespace);

	IDmemspace = H5Screate_simple( 2, idcount, NULL );

	iddataspace_id= H5Dget_space(iddset_id);
	H5Dwrite( iddset_id, hdf_float, H5S_ALL, H5S_ALL, H5P_DEFAULT, phk );

	H5Sclose(iddataspace_id);
	H5Sclose(IDmemspace);
	H5Dclose(iddset_id);

	idcount[1]=1;
	idfilespace = H5Screate_simple( 2, idcount, NULL );
	iddset_id = H5Dcreate( gid, "Psi_r", hdf_float, idfilespace, H5P_DEFAULT );
	H5Sclose(idfilespace);

	IDmemspace = H5Screate_simple( 2, idcount, NULL );

	iddataspace_id= H5Dget_space(iddset_id);
	H5Dwrite( iddset_id, hdf_float, H5S_ALL, H5S_ALL, H5P_DEFAULT, ph );

	H5Sclose(iddataspace_id);
	H5Sclose(IDmemspace);
	H5Dclose(iddset_id);

	idfilespace = H5Screate_simple( 2, idcount, NULL );
	iddset_id = H5Dcreate( gid, "delta_r", hdf_float, idfilespace, H5P_DEFAULT );
	H5Sclose(idfilespace);

	IDmemspace = H5Screate_simple( 2, idcount, NULL );

	iddataspace_id= H5Dget_space(iddset_id);
	H5Dwrite( iddset_id, hdf_float, H5S_ALL, H5S_ALL, H5P_DEFAULT, helper );

	H5Sclose(iddataspace_id);
	H5Sclose(IDmemspace);
	H5Dclose(iddset_id);

	//close "particle" group
	H5Gclose(gid);

        //close overall file
        H5Fclose(file_id);

	return 0;
}
#endif


int SaveGadget2(const char *filename, long n, io_header_2 header1, MyFloat* Pos1, MyFloat* Vel1, MyFloat* Pos2, MyFloat* Vel2, MyFloat* Pos3, MyFloat* Vel3) {
  FILE* fd = fopen(filename, "w");

  MyFloat* Pos=(MyFloat*)calloc(3,sizeof(MyFloat));
  int dummy;
  long i;
  //header block
  dummy= sizeof(header1);
  my_fwrite(&dummy, sizeof(dummy), 1, fd);
  my_fwrite(&header1, sizeof(header1), 1, fd);
  my_fwrite(&dummy, sizeof(dummy), 1, fd);

  //position block
  dummy=sizeof(MyFloat)*(long)(n*n*n)*3; //this will be 0 or some strange number for n>563; BUT: gagdget does not actually use this value; it gets the number of particles from the header
  my_fwrite(&dummy, sizeof(dummy), 1, fd);
  for(i=0;i<n*n*n;i++){
    Pos[0]=Pos1[i];
    Pos[1]=Pos2[i];
    Pos[2]=Pos3[i];
    my_fwrite(Pos,sizeof(MyFloat),3,fd);
  }
  my_fwrite(&dummy, sizeof(dummy), 1, fd);

  //velocity block
  //dummy=sizeof(MyFloat)*3*n*n*n; //this will be 0 or some strange number for n>563; BUT: gagdget does not actually use this value; it gets the number of particles from the header
  my_fwrite(&dummy, sizeof(dummy), 1, fd);
  for(i=0;i<n*n*n;i++){
    Pos[0]=Vel1[i];
    Pos[1]=Vel2[i];
    Pos[2]=Vel3[i];
   my_fwrite(Pos,sizeof(MyFloat),3,fd);
  }

  free(Pos);
  my_fwrite(&dummy, sizeof(dummy), 1, fd);

  dummy = sizeof(long) * n*n*n; //here: gadget just checks if the IDs are ints or long longs; still the number of particles is read from the header file
  my_fwrite(&dummy, sizeof(dummy), 1, fd);
  for(i=0;i<n*n*n;i++){
  my_fwrite(&i, sizeof(long), 1, fd);

  }
  my_fwrite(&dummy, sizeof(dummy), 1, fd);

  fclose(fd);

 return 0;
}

int SaveGadget3(const char *filename, long n, io_header_3 header1, MyFloat* Pos1, MyFloat* Vel1, MyFloat* Pos2, MyFloat* Vel2, MyFloat* Pos3, MyFloat* Vel3) {

    FILE* fd = fopen(filename, "w");

    MyFloat* Pos=(MyFloat*)calloc(3,sizeof(MyFloat));
    int dummy;
    long i;

    //header block
    dummy= sizeof(header1);
    my_fwrite(&dummy, sizeof(dummy), 1, fd);
    my_fwrite(&header1, sizeof(header1), 1, fd);
    my_fwrite(&dummy, sizeof(dummy), 1, fd);

    //position block
    dummy=sizeof(MyFloat)*(long)(n*n*n)*3; //this will be 0 or some strange number for n>563; BUT: gagdget does not actually use this value; it gets the number of particles from the header
    my_fwrite(&dummy, sizeof(dummy), 1, fd);
    for(i=0;i<n*n*n;i++){
	Pos[0]=Pos1[i];
    	Pos[1]=Pos2[i];
        Pos[2]=Pos3[i];
        my_fwrite(Pos,sizeof(MyFloat),3,fd);
     }

     my_fwrite(&dummy, sizeof(dummy), 1, fd);

    //velocity block
    my_fwrite(&dummy, sizeof(dummy), 1, fd);
    for(i=0;i<n*n*n;i++){
        Pos[0]=Vel1[i];
        Pos[1]=Vel2[i];
        Pos[2]=Vel3[i];
        my_fwrite(Pos,sizeof(MyFloat),3,fd);
       }

    my_fwrite(&dummy, sizeof(dummy), 1, fd);

    //particle block
    //long long ido;
    dummy = sizeof(long) * n*n*n; //here: gadget just checks if the IDs are ints or long longs; still the number of particles is read from the header file
    my_fwrite(&dummy, sizeof(dummy), 1, fd);
    for(i=0;i<n*n*n;i++){
         my_fwrite(&i, sizeof(long), 1, fd);
    }
    my_fwrite(&dummy, sizeof(dummy), 1, fd);

    fclose(fd);
    free(Pos);

  return 0;
}

int GetBuffer(double *inarray, const char *file, int insize){
   FILE *Quelle;
   int i;
   int r=0;

   Quelle = fopen(file, "r");
   if (Quelle == NULL)
      {printf("\n Input file %s not found!\n", file); exit(-1);}

  else
   { for (i = 0; i < insize*2; i++)
      {
         r=fscanf(Quelle, "%lf \n",&inarray[i]);

      }
	}
	fclose(Quelle);

  return r;
}


int GetBuffer_long(long *inarray, const char *file, int insize){
   FILE *Quelle;
   int i;
   int r=0;

   Quelle = fopen(file, "r");
   if (Quelle == NULL)
      {printf("\n Input file %s not found!\n", file); exit(-1);}

  else
   { for (i = 0; i < insize; i++)
      {
         r=fscanf(Quelle, "%lu \n", &inarray[i]);

      }
	}
	fclose(Quelle);

  return r;
}

int GetBuffer_int(int *inarray, const char *file, int insize){
   FILE *Quelle;
   int i;
   int r=0;
   float *re=new float [1];

   Quelle = fopen(file, "r");
   if (Quelle == NULL)
      {printf("\n Input file %s not found!\n", file); exit(-1);}

  else
   { for (i = 0; i < insize; i++)
      {
         r=fscanf(Quelle, "%f \n", re);
	 inarray[i]=int(re[0]);

      }
	}
	fclose(Quelle);

  return r;
}

int AllocAndGetBuffer_int(const char* IDfile, int *&pArr, int append) {
    // count lines, allocate space, then read

    FILE *f;
    int i=0;
    int r=0;
    int c;

    f = fopen(IDfile, "r");
    if (f == NULL) {printf("\n Input file %s not found!\n", IDfile); exit(-1);}

    while ( (c=fgetc(f)) != EOF ) {
        if ( c == '\n' )
                i++;
    }

    fclose(f);

    cerr << "File " <<IDfile << " has " << i << " lines" << endl;


    int *arr = (int*)calloc(i+append,sizeof(int));

    GetBuffer_int(arr, IDfile, i);

    // copy old particles into new buffer
    if (append>0)
      cerr << "Also keeping " << append << " existing particles" << endl;
    for(int j=0; j<append; j++)
      arr[i+j] = pArr[j];

    if(pArr!=NULL)
      free(pArr);

    pArr = arr;

    return i+append;
}

string make_base( string basename, int res, MyFloat box, MyFloat zin){
  ostringstream result;
#ifndef DOUBLEPRECISION
  result << basename<<"IC_iter_sing_z"<<zin<<"_"<<res<<"_L" << box;
#else
  result << basename<<"IC_iter_doub_z"<<zin<<"_"<<res<<"_L"<< box;
#endif

  return result.str();

}

MyFloat sig(MyFloat R, double *kcamb, double *Tcamb, MyFloat ns, MyFloat L, int res, int quoppa) {

  MyFloat s=0.,k,t;

  MyFloat amp=9./2./M_PI/M_PI;
  MyFloat kmax=kcamb[quoppa-1];
  MyFloat kmin=kcamb[0];

  gsl_interp_accel *acc = gsl_interp_accel_alloc ();
  gsl_spline *spline = gsl_spline_alloc (gsl_interp_cspline, quoppa);
  gsl_spline_init (spline, kcamb, Tcamb, quoppa);

  MyFloat dk=(kmax-kmin)/10000.;
    for(k=kmin; k<kmax;k+=dk){

	t=gsl_spline_eval (spline, k, acc);

	s+= powf(k,ns+2.)*((sin(k*R)-k*R*cos(k*R))/((k*R)*(k*R)*(k*R)) ) *( (sin(k*R)-k*R*cos(k*R))/((k*R)*(k*R)*(k*R)) )*t*t;

    }

  gsl_spline_free (spline);
  gsl_interp_accel_free (acc);

  s=sqrt(s*amp*dk);
  return s;

}

MyFloat D(MyFloat a, MyFloat Om, MyFloat Ol){

   MyFloat Hsq=Om/powf(a,3.0)+(1.-Om-Ol)/a/a+Ol;
   MyFloat d=2.5*a*Om/powf(a,3.0)/Hsq/( powf(Om/Hsq/a/a/a,4./7.) - Ol/Hsq + (1.+0.5*Om/powf(a,3.0)/Hsq)*(1.+1./70.*Ol/Hsq) );

   //simplify this...?

 return d;
}


complex<MyFloat> *fft_r(complex<MyFloat> *fto, complex<MyFloat> *ftin, const int res, const int dir){ //works when fto and ftin and output are the same, but overwrites fto for the backwards transform so we have another temporary array there

    long i;

  if(dir==1){
	std::complex<MyFloat> *fti = (std::complex<MyFloat>*)calloc(res*res*res,sizeof(std::complex<MyFloat>));
	for(i=0;i<res*res*res;i++){fti[i]=ftin[i]/sqrt((MyFloat)(res*res*res));}
	//for(i=0;i<res*res*res;i++){fti[i]=ftin[i]/(MyFloat)(res*res*res);}
	fftwnd_plan pf = fftw3d_create_plan( res, res, res, FFTW_FORWARD, FFTW_ESTIMATE );
	fftwnd_one(pf, reinterpret_cast<fftw_complex*>(&fti[0]), reinterpret_cast<fftw_complex*>(&fto[0]));
	fftwnd_destroy_plan(pf);
	free(fti);
	    }

  else if(dir==-1){
	std::complex<MyFloat> *fti = (std::complex<MyFloat>*)calloc(res*res*res,sizeof(std::complex<MyFloat>));
	//memcpy(fti,ftin,res*res*res*sizeof(fftw_complex));
	for(i=0;i<res*res*res;i++){fti[i]=ftin[i]/sqrt((MyFloat)(res*res*res));}
	fftwnd_plan pb;
 	pb = fftw3d_create_plan( res,res,res, FFTW_BACKWARD, FFTW_ESTIMATE );
	fftwnd_one(pb, reinterpret_cast<fftw_complex*>(&fti[0]), reinterpret_cast<fftw_complex*>(&fto[0]));
	fftwnd_destroy_plan(pb);
	free(fti);
	  }

  else {std::cerr<<"wrong parameter for direction in fft_r"<<std::endl;}

  return fto;
}

complex<MyFloat> dot(complex<MyFloat> *a, complex<MyFloat> *b, const long int n) {
  complex<MyFloat> res(0);
  for(long int i=0; i<n; i++) res+=conj(a[i])*b[i];
  return res;
}

complex<MyFloat> chi2(complex<MyFloat> *a, complex<MyFloat> *b, const long int n) {
    complex<MyFloat> res(0);
    for(long int i=1; i<n; i++) // go from 1 to avoid div by zero
    {
        res+=conj(a[i])*a[i]/b[i];
    }
    return res/((MyFloat)n);
}

void mat_diag(const complex<MyFloat> *C, const complex<MyFloat> *alpha, const long int n, const complex<MyFloat> p, complex<MyFloat> *result){

  long int i;
  for(i=0;i<n;i++){ result[i]=C[i]*alpha[i]*p;}

}

void mat_mat_diag(const complex<MyFloat> *z, const complex<MyFloat> *alpha, const long int n, const complex<MyFloat> p, complex<MyFloat> *result){

  long int i;
  complex<MyFloat> tf=0.;
  for(i=0;i<n;i++){
    tf+=z[i]*conj(alpha[i]);
  }
  for(i=0;i<n;i++){
    result[i]=alpha[i]*tf*p;
  }


}

void mat_sum(const complex<MyFloat> *C1, const complex<MyFloat> *C2, const long int n, const complex<MyFloat> p, complex<MyFloat> *result){

  long int i;
  for(i=0;i<n;i++){
    result[i]=p*(C1[i]+C2[i]);
  }

}


complex<MyFloat>* calc_A_new(complex<MyFloat>* z, const complex<MyFloat> *alpha, const long int n, const complex<MyFloat> *C0, complex<MyFloat> *temp, complex<MyFloat> *temp2, complex<MyFloat> *temp3){
  // Added by AP - non-iterative version... sorry! :-/
  mat_mat_diag(z,alpha,n, complex<MyFloat>(-1.0,0.), temp);
  return temp;
}

complex<MyFloat>* calc_A(int iter, complex<MyFloat>* z, const complex<MyFloat> *alpha, const long int n, const complex<MyFloat> *C0, complex<MyFloat> *temp, complex<MyFloat> *temp2, complex<MyFloat> *temp3){

  if(iter==0){
    mat_mat_diag(z,alpha,n, complex<MyFloat>(-0.5,0.), temp);
    return temp;
  }
  else{

  temp3=calc_A(iter-1,z, alpha, n, C0, temp, temp2, temp3);

  complex<MyFloat> *md=(complex<MyFloat>*)calloc(n,sizeof(complex<MyFloat>));

  mat_diag(C0,temp3, n, complex<MyFloat>(1.,0.), md);

  temp2=calc_A(iter-1, md, alpha, n, C0, temp, temp3, temp2);

  mat_mat_diag(z,alpha,n, complex<MyFloat>(1.,0.), md);
  mat_sum(temp2, md, n, complex<MyFloat>(-0.5,0.), temp3);

  free(md);

  return temp3;

  }
}


void ret_exp(long res, MyFloat R, MyFloat mr, MyFloat L, std::complex<MyFloat> *ret){//generates a Gaussian constraint vector

  long r1, r2, r3, i;//, rr1, rr2, rr3;
  MyFloat r, w, n_r=0.;

  MyFloat rw=1./MyFloat(res)*L;

        for(r1=0; r1<res;r1++){
  	  for(r2=0;r2<res;r2++){
      	    for(r3=0;r3<res;r3++){

		i = (r1*res+r2)*(res)+r3;
		r=MyFloat( ( (r1-mr)*(r1-mr) + (r2-mr)*(r2-mr) + (r3-mr)*(r3-mr) ) );
		w=(MyFloat)exp(-(r/R/R*rw*rw/2.));

		ret[i].real(w);
		n_r+=w;

	    }
	  }
	}

     for(i=0; i<res*res*res; i++){ ret[i]/=n_r;}

}


//faster than sorting, even though we interpolate more often
void brute_interpol_new(int res, double *kcamb, double *Tcamb, int quoppa, MyFloat kw, MyFloat ns, MyFloat norm_amp, complex<MyFloat> *ft, complex<MyFloat> *ftsc, complex<MyFloat> *P){

  complex<MyFloat> norm_iter (0.,0.);

  gsl_interp_accel *acc = gsl_interp_accel_alloc ();
  gsl_spline *spline = gsl_spline_alloc (gsl_interp_cspline, quoppa);
  gsl_spline_init (spline, kcamb, Tcamb, quoppa);

  MyFloat kk=0.;
  long k1,k2,k3;
  long ii,jj,ll;
  long idk;

  for(k1=-res/2; k1<res/2;k1++){
      if(k1<0){ii=k1+res;}else{ii=k1;}
     for(k2=-res/2;k2<res/2;k2++){
       if(k2<0){jj=k2+res;}else{jj=k2;}
       for(k3=-res/2;k3<res/2;k3++){
	 if(k3<0){ll=k3+res;}else{ll=k3;}


	 idk=(ii*(res)+jj)*(res)+ll;
	 if(idk==0){continue;}
	 kk=sqrt(k1*k1+k2*k2+k3*k3)*kw;

	 P[idk]=gsl_spline_eval (spline, kk, acc);
	 P[idk]*=(P[idk]* powf(kk,ns) *norm_amp );

	 ftsc[idk]=sqrt(P[idk])*ft[idk];

       }
    }
  }


  gsl_spline_free (spline);
  gsl_interp_accel_free (acc);

}


void powsp(int n, complex<MyFloat>* ft, const char *out, MyFloat Boxlength){

 int res=n;
 int nBins=100;
 MyFloat *inBin = new MyFloat[nBins];
 MyFloat *Gx = new MyFloat[nBins];
 MyFloat *kbin = new MyFloat[nBins];
 MyFloat kmax = M_PI/Boxlength*(MyFloat)res, kmin = 2.0f*M_PI/(MyFloat)Boxlength, dklog = log10(kmax/kmin)/nBins, kw = 2.0f*M_PI/(MyFloat)Boxlength;

      int ix,iy,iz,idx,idx2;
      MyFloat kfft;

      for( ix=0; ix<nBins; ix++ ){
	inBin[ix] = 0;
	Gx[ix] = 0.0f;
	kbin[ix] = 0.0f;
      }


   for(ix=0; ix<res;ix++)
    for(iy=0;iy<res;iy++)
      for(iz=0;iz<res;iz++){
        idx = (ix*res+iy)*(res)+iz;

        // determine mode modulus

	MyFloat vabs = ft[idx].real()*ft[idx].real()+ft[idx].imag()*ft[idx].imag();

	int iix, iiy, iiz;

        if( ix>res/2 ) iix = ix - res; else iix = ix;
        if( iy>res/2 ) iiy = iy - res; else iiy = iy;
        if( iz>res/2 ) iiz = iz - res; else iiz = iz;

        kfft = sqrt(iix*iix+iiy*iiy+iiz*iiz);
       MyFloat k = kfft*kw;

        // correct for aliasing, formula from Jing (2005), ApJ 620, 559
        // assume isotropic aliasing (approx. true for k<kmax=knyquist)
        // this formula is for CIC interpolation scheme, which we use <- only needed for Powerspectrum

       MyFloat JingCorr = (1.0f-2.0f/3.0f*sin(M_PI*k/kmax/2.0f)*sin(M_PI*k/kmax/2.0f));
	vabs /= JingCorr;

        //.. logarithmic spacing in k
	      idx2 = (int)(( 1.0f/dklog * log10(k/kmin) )); //cout<<idx2<<endl;

        if(k>=kmin&&k<kmax){


            Gx[idx2] += vabs;
            kbin[idx2] += k;
            inBin[idx2]++;

        }else{continue;}

      }


  //... convert to physical units ...
  std::ofstream ofs(out);

  //definition of powerspectrum brings (2pi)^-3, FT+conversion to physical units brings sqrt(Box^3/N^6) per delta1, where ps22~d2*d2~d1*d1*d1*d1 -> (Box^3/N^6)^2

	MyFloat psnorm=powf(Boxlength/(2.0*M_PI),3.0);

  for(ix=0; ix<nBins; ix++){

	if(inBin[ix]>0){


    ofs << std::setw(16) << pow (10., log10 (kmin) + dklog * (ix + 0.5) )
	<< std::setw(16) <<  kbin[ix]/inBin[ix]
        << std::setw(16) << (MyFloat)(Gx[ix]/inBin[ix])*psnorm
        << std::setw(16) << inBin[ix]
        << std::endl;

	}
  }
  ofs.close();

  free(inBin);
  free(kbin);
  free(Gx);

}

void powsp_noJing(int n, complex<MyFloat>* ft, const char *out, MyFloat Boxlength){

 int res=n;
 int nBins=100;
 MyFloat *inBin = new MyFloat[nBins];
 MyFloat *Gx = new MyFloat[nBins];
 MyFloat *kbin = new MyFloat[nBins];
 MyFloat kmax = M_PI/Boxlength*(MyFloat)res, kmin = 2.0f*M_PI/(MyFloat)Boxlength, dklog = log10(kmax/kmin)/nBins, kw = 2.0f*M_PI/(MyFloat)Boxlength;

      int ix,iy,iz,idx,idx2;
      MyFloat kfft;

      for( ix=0; ix<nBins; ix++ ){
	inBin[ix] = 0;
	Gx[ix] = 0.0f;
	kbin[ix] = 0.0f;
      }


   for(ix=0; ix<res;ix++)
    for(iy=0;iy<res;iy++)
      for(iz=0;iz<res;iz++){
        idx = (ix*res+iy)*(res)+iz;

        // determine mode modulus

	MyFloat vabs = ft[idx].real()*ft[idx].real()+ft[idx].imag()*ft[idx].imag();

	int iix, iiy, iiz;

        if( ix>res/2 ) iix = ix - res; else iix = ix;
        if( iy>res/2 ) iiy = iy - res; else iiy = iy;
        if( iz>res/2 ) iiz = iz - res; else iiz = iz;

        kfft = sqrt(iix*iix+iiy*iiy+iiz*iiz);
       MyFloat k = kfft*kw;

        //.. logarithmic spacing in k
	      idx2 = (int)(( 1.0f/dklog * log10(k/kmin) ));

        if(k>=kmin&&k<kmax){

            Gx[idx2] += vabs/(MyFloat)(res*res*res); //because FFT is now normalised with 1/sqrt(Ntot)
            kbin[idx2] += k;
            inBin[idx2]++;

        }else{continue;}

      }


  //... convert to physical units ...
  std::ofstream ofs(out);


	MyFloat psnorm=powf(Boxlength/(2.0*M_PI),3.0);

  for(ix=0; ix<nBins; ix++){

	if(inBin[ix]>0){


    ofs << std::setw(16) << pow (10., log10 (kmin) + dklog * (ix + 0.5) )
	<< std::setw(16) <<  kbin[ix]/inBin[ix]
        << std::setw(16) << (MyFloat)(Gx[ix]/inBin[ix])*psnorm
        << std::setw(16) << inBin[ix]
        << std::endl;

	}
  }
  ofs.close();

  free(inBin);
  free(kbin);
  free(Gx);

}

complex<MyFloat> *poiss(complex<MyFloat>* out, complex<MyFloat> *in, int res, MyFloat Boxlength, MyFloat a, MyFloat Om){

  long i;
  MyFloat prefac=3./2.*Om/a*100.*100./(3.*100000.)/(3.*100000.); // =3/2 Om0/a * (H0/h)^2 (h/Mpc)^2 / c^2 (km/s)
  MyFloat  kw = 2.0f*M_PI/(MyFloat)Boxlength,k;

  int k1,k2,k3,kk1,kk2,kk3;

   for(k1=0; k1<res;k1++){
       for(k2=0;k2<res;k2++){
          for(k3=0;k3<res;k3++){
        	i = (k1*res+k2)*(res)+k3;

		if( k1>res/2 ) kk1 = k1-res; else kk1 = k1;
        	if( k2>res/2 ) kk2 = k2-res; else kk2 = k2;
 		if( k3>res/2 ) kk3 = k3-res; else kk3 = k3;

		k=(MyFloat)(kk1*kk1+kk2*kk2+kk3*kk3)*kw*kw;

		out[i]=-in[i]*prefac/k;
	  }
       }
   }

   out[0]=complex<MyFloat>(0.,0.);

   return out;

}

complex<MyFloat> *rev_poiss(complex<MyFloat> *out, complex<MyFloat> *in, int res, MyFloat Boxlength, MyFloat a, MyFloat Om){

  long i;

  MyFloat prefac=3./2.*Om/a*100.*100./(3.*100000.)/(3.*100000.); // 3/2 Om/a * (H0/h)^2 (h/Mpc)^2 / c^2 (km/s)

  MyFloat  kw = 2.0f*M_PI/(MyFloat)(Boxlength),k;

  int k1,k2,k3,kk1,kk2,kk3;

   for(k1=0; k1<res;k1++){
      for(k2=0;k2<res;k2++){
         for(k3=0;k3<res;k3++){
	    i = (k1*res+k2)*(res)+k3;

		if( k1>res/2 ) kk1 = k1-res; else kk1 = k1;
        	if( k2>res/2 ) kk2 = k2-res; else kk2 = k2;
 		if( k3>res/2 ) kk3 = k3-res; else kk3 = k3;

		k=(MyFloat)(kk1*kk1+kk2*kk2+kk3*kk3)*kw*kw;

		out[i]=-k*in[i]/prefac;
	 }
      }
   }

    out[0]=complex<MyFloat> (0.,0.);

   return out;

}

void pbc(long n, std::complex<MyFloat> *delta_in, std::complex<MyFloat> *delta_out, long r1, long r2, long r3, long *index_shift){

  if(abs(r1) > n ||  abs(r2) > n || abs(r3) > n ){std::cerr << "Wrong shift parameters: " << r1 << " "<< r2 << " "<<r3 <<std::endl; exit(1);}

  long ix,iy,iz,id1,id2,jx,jy,jz,jjx,jjy,jjz;
  for(ix=0;ix<n;ix++){
    for(iy=0;iy<n;iy++){
      for(iz=0;iz<n;iz++){

	       id1=(ix*n+iy)*n+iz;

	       jx=ix-r1;
	       jy=iy-r2;
	       jz=iz-r3;

	       if( jx>n-1 ) jjx = jx - n; else if(jx <0) jjx = jx + n; else jjx = jx;
	       if( jy>n-1 ) jjy = jy - n; else if(jy <0) jjy = jy + n; else jjy = jy;
	       if( jz>n-1 ) jjz = jz - n; else if(jz <0) jjz = jz + n; else jjz = jz;

	       id2=(jjx*n+jjy)*n+jjz;

	       delta_out[id2]=delta_in[id1];
         index_shift[id1]=id2;

      }
    }
  }


}

void GridParticles( int nx, int ny, int nz, MyFloat *Pos1,MyFloat *Pos2,MyFloat *Pos3, MyFloat Boxlength, unsigned nPart, MyFloat m,  fftw_real *data , MyFloat subbox, int *ourbox){

  unsigned ix, iy, iz, i;

  MyFloat wpar =  (MyFloat)(nx*ny*nz)/(MyFloat)nPart;

  MyFloat x,y,z,dx,dy,dz,tx,ty,tz,tyw,dyw;
  unsigned ix1,iy1,iz1;
  int icount = 0;
  unsigned nPartThis = nPart;

  for (i = 0; i < 3; i++)
      if (ourbox[i] > subbox || ourbox[i] < 1){cerr<< "Error with ourbox."; exit(-1);}
	  //error (-1,0,"Error with ourbox.");

  for( unsigned i=0; i<nPartThis; ++i ){
    x = Pos1[i];
    y = Pos2[i];
    z = Pos3[i];



    x -= (ourbox[0]-1.) * (MyFloat)nx;
    y -= (ourbox[1]-1.) * (MyFloat)ny;
    z -= (ourbox[2]-1.) * (MyFloat)nz;

      if (x < nx && y < ny && z < nz && x > 0. && y > 0. && z > 0.){
	  ++icount;
      }
  }

  wpar =  (MyFloat)(nx*ny*nz)/(MyFloat)nPart;


  for( unsigned i=0; i<nPartThis; ++i ){

    x = Pos1[i];
    y = Pos2[i];
    z = Pos3[i];


    x -= (ourbox[0]-1.) * (MyFloat)nx;
    y -= (ourbox[1]-1.) * (MyFloat)ny;
    z -= (ourbox[2]-1.) * (MyFloat)nz;


    if (x < nx && y < ny && z < nz && x > 0. && y > 0. && z > 0.){


    ix = (unsigned)x;
    iy = (unsigned)y;
    iz = (unsigned)z;

    dx = (x-((MyFloat)ix));
    dy = (y-((MyFloat)iy));
    dz = (z-((MyFloat)iz));

    ix %= nx;
    iy %= ny;
    iz %= nz;

    tx = 1.0f-dx;
    ty = 1.0f-dy;
    tz = 1.0f-dz;

    tyw = ty*wpar;
    dyw = dy*wpar;

    ix1 = (ix+1)%nx;
    iy1 = (iy+1)%ny;
    iz1 = (iz+1)%nz;

    data[(ix*ny + iy) * nz + iz]   += tz*tx*tyw;
    data[(ix1*ny + iy) * nz + iz]  += tz*dx*tyw;
    data[(ix*ny + iy1) * nz + iz]  += tz*tx*dyw;
    data[(ix1*ny + iy1) * nz + iz] += tz*dx*dyw;

    data[(ix*ny + iy) * nz + iz1]   += dz*tx*tyw;
    data[(ix1*ny + iy) * nz + iz1]  += dz*dx*tyw;
    data[(ix*ny + iy1) * nz + iz1]  += dz*tx*dyw;
    data[(ix1*ny + iy1) * nz + iz1] += dz*dx*dyw;
    }

  }
}

void old_check_pbc_coords(long *coords, int size){

  while(coords[0]> size){coords[0]-=size;}
  while(coords[1]> size){coords[1]-=size;}
  while(coords[2]> size){coords[2]-=size;}

  while(coords[0]<0){coords[0]+=size;}
  while(coords[1]<0){coords[1]+=size;}
  while(coords[2]<0){coords[2]+=size;}


}

struct grid_struct{

  long grid[3];
  long coords[3];
  MyFloat absval;
  MyFloat delta;

};


class Grid{

  //later: think about making some variables/functions private
  //private:
    //int size;

  public:
    //constructor:
    Grid(int s);
    int size;
    grid_struct* cells;

    ~Grid() {free(cells);}

    void shift_grid(long s1, long s2, long s3);
    long find_next_ind(long index, int *step);
    long get_index(long x, long y, long z);
    //void check_pbc_grid(long index);
    void check_pbc_grid(long *grid);
    void check_pbc_coords(long index);

};

Grid::Grid(int n){

  cout<< "Constructing grid class" << endl;
  size=n;

  grid_struct *cells=(grid_struct*)calloc(n*n*n,sizeof(grid_struct));
  long g1,g2,g3, gg1,gg2,gg3, ind;
  MyFloat absval;
    for(g1=0;g1<n;g1++){
      gg1=g1;
      if(g1>n/2) gg1=g1-n;
	for(g2=0;g2<n;g2++){
	  gg2=g2;
	  if(g2>n/2) gg2=g2-n;
	    for(g3=0;g3<n;g3++){
	      gg3=g3;
	      if(g3>n/2) gg3=g3-n;

	      ind=(g1*n+g2)*n+g3;
	      absval=sqrt(gg1*gg1+gg2*gg2+gg3*gg3);
	      cells[ind].absval=absval;
	      cells[ind].coords[0]=gg1;
	      cells[ind].coords[1]=gg2;
	      cells[ind].coords[2]=gg3;
	      cells[ind].grid[0]=g1;
	      cells[ind].grid[1]=g2;
	      cells[ind].grid[2]=g3;
        //if(ind==0){cout<< "in builder "<< cells[0].grid[0] << " "<< cells[0].grid[1] << " "<<cells[0].grid[2]<< endl;} //correct here

	    }
	  }
	}

	this->cells=cells;

}

long Grid::find_next_ind(long index, int *step){

  long grid[3]={0,0,0};

  grid[0]=this->cells[index].grid[0];//+step[0];
  grid[1]=this->cells[index].grid[1];//+step[1];
  grid[2]=this->cells[index].grid[2];//+step[2];

  // cout << "in finder before sum "<< grid[0] << " " << grid[1] << " "<< grid[2]  <<endl;
  // cout << this->cells[0].grid[0]<< " "<< this->cells[0].grid[1] << " " << this->cells[0].grid[2] <<endl;
  // cout << endl;

  grid[0]=this->cells[index].grid[0]+step[0];
  grid[1]=this->cells[index].grid[1]+step[1];
  grid[2]=this->cells[index].grid[2]+step[2];

  // cout << "in finder after sum "<< grid[0] << " " << grid[1] << " "<< grid[2]  <<endl;
  // cout << this->cells[0].grid[0]<< " "<< this->cells[0].grid[1] << " " << this->cells[0].grid[2] <<endl;
  // cout<< endl;

  this->check_pbc_grid(grid);

  // cout << "after pbc "<< grid[0] << " " << grid[1] << " "<< grid[2]  <<endl;
  // cout << this->cells[0].grid[0]<< " "<< this->cells[0].grid[1] << " " << this->cells[0].grid[2] <<endl;
  // cout<<endl;

  long newind;

  newind=this->get_index(grid[0], grid[1], grid[2]);

  //delete grid;

  return newind;

}

void Grid::check_pbc_grid(long *grid){

 long x=grid[0];
 long y=grid[1];
 long z=grid[2];

 //cout << "in pbc " <<  x << " " << y << " " << z << endl;

 int size=this->size;

  while(x> size-1){x-=size;}
  while(y> size-1){y-=size;}
  while(z> size-1){z-=size;}

  while(x<0){x+=size;}
  while(y<0){y+=size;}
  while(z<0){z+=size;}

  //cout << "in pbc after " <<  x << " " << y << " " << z << endl;

  grid[0]=x;
  grid[1]=y;
  grid[2]=z;

  //cout << "in pbc final" <<  grid[0] << " " << grid[1] << " " << grid[2] << endl;
}

// void Grid::check_pbc_grid_old(long index){

//  long x=this->cells[index].grid[0];
//  long y=this->cells[index].grid[1];
//  long z=this->cells[index].grid[2];

//  int size=this->size;

//   //todo: replace this with single cases with signum or something?

//   while(x> size){x-=size;}
//   while(y> size){y-=size;}
//   while(z> size){z-=size;}

//   while(x<0){x+=size;}
//   while(y<0){y+=size;}
//   while(z<0){z+=size;}

//   this->cells[index].grid[0]=x;
//   this->cells[index].grid[1]=y;
//   this->cells[index].grid[2]=z;

// }

void Grid::check_pbc_coords(long index){

 long x=this->cells[index].coords[0];
 long y=this->cells[index].coords[1];
 long z=this->cells[index].coords[2];

 int size=this->size;

 while(x> size/2){x-=size;}
 while(y> size/2){y-=size;}
 while(z> size/2){z-=size;}

 while(x<-(size/2-1)){x+=size;}
 while(y<-(size/2-1)){y+=size;}
 while(z<-(size/2-1)){z+=size;}

 this->cells[index].coords[0]=x;
 this->cells[index].coords[1]=y;
 this->cells[index].coords[2]=z;


}

void Grid::shift_grid(long s0, long s1, long s2){

  //long coords[3];
  long index;
  long max=(this->size)*(this->size)*(this->size);

  for(index=0; index< max; index++){
     if(index==0 || index ==1 || index==max-1){ cout<< "before: index: "<< index << " " << this->cells[index].coords[0] << " " << this->cells[index].coords[1] << " "<< this->cells[index].coords[2] << endl;}

      this->cells[index].coords[0]-=s0;
      this->cells[index].coords[1]-=s1;
      this->cells[index].coords[2]-=s2;

      if(index==0 || index ==1 || index==max-1){ cout<< "intermed: index: "<< index << " " << this->cells[index].coords[0] << " " << this->cells[index].coords[1] << " "<< this->cells[index].coords[2] << endl;}

      this->check_pbc_coords(index);

      if(index==0 || index ==1 || index==max-1){ cout<< "after: index: "<< index << " " << this->cells[index].coords[0] << " " << this->cells[index].coords[1] << " "<< this->cells[index].coords[2] << endl;}

  }

  //free(coords);

}

long Grid::get_index(long x, long y, long z){

  long size=this->size;
  long index=(x*size+y)*size+z;

  return index;

}

MyFloat get_wrapped_delta(MyFloat x0, MyFloat x1, MyFloat boxlen) {
    MyFloat result = x0-x1;
    if(result>boxlen/2) {
        result-=boxlen;
    }
    if(result<-boxlen/2) {
        result+=boxlen;
    }
    return result;
}

void cen_deriv4_alpha(Grid *in, long index, complex<MyFloat> *alpha, MyFloat dx, long direc,
		      MyFloat xc, MyFloat yc, MyFloat zc, MyFloat boxlen){//4th order central difference

  MyFloat x0, y0, z0;//, zm2, zm1, zp2, zp1;
  x0=get_wrapped_delta(dx*in->cells[index].coords[0],xc,boxlen);
  y0=get_wrapped_delta(dx*in->cells[index].coords[1],yc,boxlen);
  z0=get_wrapped_delta(dx*in->cells[index].coords[2],zc,boxlen);

   // cout << "index "<< index << " (x,y,z) "<< x0<< " " << y0 <<" "<< z0 << endl;

  complex<MyFloat> ang=0.;

  //do the epsilon
  MyFloat c1,c2;
  long d1,d2;
  if(direc==0){d2=1; d1=2; c1=y0; c2=z0;}
  else if(direc==1){d2=2; d1=0, c1=z0; c2=x0;}
  else if(direc==2){d2=0; d1=1; c1=x0; c2=y0;}
  else{cerr<< "Wrong value for parameter 'direc' in function 'cen_deriv4_alpha'."<< endl; exit(1);}

  long ind_p1, ind_m1, ind_p2, ind_m2;
  //first step in rho direction
    int step1[3]={0,0,0};
    step1[d1]=1;
    int neg_step1[3]={0,0,0};
    neg_step1[d1]=-1;

    ind_m1=in->find_next_ind(index, neg_step1);
    ind_p1=in->find_next_ind(index, step1);

    //cout << "m1 " << ind_m1 << " " << in->cells[ind_m1].coords[0] << " "<< in->cells[ind_m1].coords[1]  << " "<< in->cells[ind_m1].coords[2] << endl;
    //cout << "p1 " << ind_p1 << " " << in->cells[ind_p1].coords[0] << " "<< in->cells[ind_p1].coords[1]  << " "<< in->cells[ind_p1].coords[2] << endl;

    ind_m2=in->find_next_ind(ind_m1, neg_step1);
    ind_p2=in->find_next_ind(ind_p1, step1);

    //cout << "m2 " << ind_m2 << " " << in->cells[ind_m2].coords[0] << " "<< in->cells[ind_m2].coords[1]  << " "<< in->cells[ind_m2].coords[2] << endl;
    //cout << "p2 " << ind_p2<< " "  << in->cells[ind_p2].coords[0] << " "<< in->cells[ind_p2].coords[1]  << " "<< in->cells[ind_p2].coords[2] << endl;

    MyFloat a=-1./12./dx, b=2./3./dx;  //the signs here so that L ~ - Nabla Phi

    alpha[ind_m2]+=(c1*a);
    alpha[ind_m1]+=(c1*b);
    alpha[ind_p1]+=(-c1*b);
    alpha[ind_p2]+=(-c1*a);



  //second step in other rho direction
    int step2[3]={0,0,0};
    step2[d2]=1;
    int neg_step2[3]={0,0,0};
    neg_step2[d2]=-1;

    ind_m1=in->find_next_ind(index, neg_step2);
    ind_p1=in->find_next_ind(index, step2);

    ind_m2=in->find_next_ind(ind_m1, neg_step2);
    ind_p2=in->find_next_ind(ind_p1, step2);

    alpha[ind_m2]+=(-c2*a);
    alpha[ind_m1]+=(-c2*b);
    alpha[ind_p1]+=(c2*b);
    alpha[ind_p2]+=(c2*a);


}

complex<MyFloat> cen_deriv2_alpha(Grid *in, int index, complex<MyFloat> *alpha, MyFloat dx, int direc, complex<MyFloat> *phi){//second order central difference

  MyFloat x0, y0, z0;
  x0=in->cells[index].coords[0];
  y0=in->cells[index].coords[1];
  z0=in->cells[index].coords[2];

  complex<MyFloat> ang=0.;

  //do the epsilon
  MyFloat c1,c2;
  int d1,d2;
  if(direc==0){d2=1; d1=2; c1=y0; c2=z0;}
  else if(direc==1){d2=2; d1=0, c1=z0; c2=x0;}
  else if(direc==2){d2=0; d1=1; c1=x0; c2=y0;}
  else{cerr<< "Wrong value for parameter 'direc' in function 'cen_deriv2_alpha'."<< endl; exit(1);}

  int ind_p1, ind_m1;
  //first step in rho direction
    int step1[3]={0,0,0};
    step1[d1]=1;
    int neg_step1[3]={0,0,0};
    neg_step1[d1]=-1;

    ind_m1=in->find_next_ind(index, neg_step1);
    ind_p1=in->find_next_ind(index, step1);

    MyFloat a=-0.5/dx;

    alpha[ind_m1]+=(c1*a);
    alpha[ind_p1]+=(-c1*a);

    ang+=((c1*a*phi[ind_m1])+(-c1*a*phi[ind_p1]));

  //second step in other rho direction
    int step2[3]={0,0,0};
    step2[d2]=1;
    int neg_step2[3]={0,0,0};
    neg_step2[d2]=-1;

    ind_m1=in->find_next_ind(index, neg_step2);
    ind_p1=in->find_next_ind(index, step2);

    alpha[ind_m1]+=(-c2*a);
    alpha[ind_p1]+=(c2*a);

    ang+=((-c2*a*phi[ind_m1])+(c2*a*phi[ind_p1]));

    return ang;

}


void getCentre(int *part_arr, int n_part_arr, MyFloat boxlen, MyFloat dx,
               MyFloat &x0, MyFloat &y0, MyFloat &z0, Grid *pGrid) {

    x0 = 0; y0 = 0; z0 =0;
    MyFloat xa=pGrid->cells[part_arr[0]].coords[0]*dx;
    MyFloat ya=pGrid->cells[part_arr[0]].coords[1]*dx;
    MyFloat za=pGrid->cells[part_arr[0]].coords[2]*dx;

    for(long i=0;i<n_part_arr;i++) {
        x0+=get_wrapped_delta(pGrid->cells[part_arr[i]].coords[0]*dx,xa,boxlen);
        y0+=get_wrapped_delta(pGrid->cells[part_arr[i]].coords[1]*dx,ya,boxlen);
        z0+=get_wrapped_delta(pGrid->cells[part_arr[i]].coords[2]*dx,za,boxlen);
    }
    x0/=n_part_arr;
    y0/=n_part_arr;
    z0/=n_part_arr;
    x0+=xa;
    y0+=ya;
    z0+=za;
}

void reorderBuffer(int *part_arr, int n_part_arr, int npartTotal, int res,
                   MyFloat dx, float a, float om, float boxlen, Grid *pGrid) {

    MyFloat r2[n_part_arr];
    MyFloat x0,y0,z0;
    getCentre(part_arr, n_part_arr, boxlen, dx, x0,y0,z0, pGrid);


}

complex<MyFloat> *calcConstraintVector(istream &inf, int *part_arr, int n_part_arr, int npartTotal, int res,
                                       MyFloat dx, float a, float om, float boxlen, Grid *pGrid) {
    char name[100];
    inf >> name;
    cout << "Getting constraint vector '" << name << "' for " << n_part_arr << " particles.";

    complex<MyFloat> *rval=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
    complex<MyFloat> *rval_k=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));

    if(strcasecmp(name,"overdensity")==0) {
        MyFloat w = 1.0/n_part_arr;
        for(long i=0;i<n_part_arr;i++) {
            rval[part_arr[i]]=w;
        }

        fft_r(rval_k, rval, res, 1);
    } else if(strcasecmp(name, "L")==0) {
        // angular momentum
        int direction=-1;
        inf >> direction;
        MyFloat x0,y0,z0;
        getCentre(part_arr, n_part_arr, boxlen, dx, x0,y0,z0, pGrid);

        cerr << "Angmom centre is " <<x0 << " " <<y0 << " " << z0 << endl;

        for(long i=0;i<n_part_arr;i++) {
            cen_deriv4_alpha(pGrid, part_arr[i], rval, dx, direction, x0, y0, z0, boxlen);
        }
        complex<MyFloat> *rval_kX=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
        fft_r(rval_kX, rval, res, 1);
        // The constraint as derived is on the potential. By considering
        // unitarity of FT, we can FT the constraint to get the constraint
        // on the density.
        poiss(rval_k, rval_kX, res, boxlen, a, om);
        free(rval_kX);
    } else {
        cout << "  -> UNKNOWN constraint vector type, returning zeros [this is bad]" << endl;

    }

    free(rval);
    return rval_k;

}


int main(int argc, char *argv[]){

  if(argc!=2)
  {
      cerr<<"Usage: ./ICgauss paramfile | Output: Pos,Vel,IDs as hdf5 and/or gadget format, power spectrum and used parameters in textfile."<<endl;
      return -1;
  }

    MyFloat Om0, Ol0, zin, sigma8, Boxlength, a1, a2, a3;
    int out, n, gadgetformat;

    int n_in_bin;
    int *part_arr=NULL;

	MyFloat in_d=1.0;
	complex<MyFloat> d (in_d,0.); //purely real to avoid headaches
    //read parameterfile
    ifstream inf;
      char flag[11][200];
      char value[15][200];
      int seed;
      string incamb;
      string indir;
     string infs=argv[1];
   ofstream parout;

    inf.open(infs.c_str());
      if(inf.is_open()){
	cout<< "Reading parameter file..." << infs.c_str() << endl;
	if(inf.peek()=='%'){inf.ignore(200, '\n');} //ignores first line, which is a comment starting with %

	inf>> flag[0] >>value[0];
	Om0=atof(value[0]);

	    inf>> flag[1] >>value[1];
	    Ol0=atof(value[1]);

	    inf>> flag[2] >>value[2];
	     sigma8=atof(value[2]);

	    inf >> flag[3]>> value[3];
	    Boxlength=atof(value[3]);

	  inf>> flag[4] >>value[4];
	   zin=atof(value[4]);

	  inf>> flag[5] >>value[5];
	   n=atoi(value[5]);

	  inf>> flag[6] >>value[6];
	   out=atoi(value[6]);

	  inf>> flag[7] >>value[7];
	  seed=atoi(value[7]);
	  inf>> flag[8] >>value[8];

	  inf>> flag[9] >>value[9];
	  string indir=value[9];

	  inf>> flag[10] >>value[10];
          gadgetformat=atoi(value[10]);


      }
      else{cerr<< "Noooooo! You fool! That file is not here!"<< endl; return -1;}


     long npartTotal=(long) (n*n*n);

     gsl_rng * r;
     const gsl_rng_type * T; //generator type variable

     //gsl_rng_env_setup(); //use this instead of gsl_rng_set(r,seed) below for seed from command line

#ifndef DOUBLEPRECISION
T = gsl_rng_ranlxs2; //this is the name of the generator defined in the environmental variable GSL_RNG_TYPE
#else
T = gsl_rng_ranlxs2; //double precision generator: gsl_rng_ranlxd2 //TODO decide which one (but better consistently)
#endif
       r = gsl_rng_alloc (T); //this allocates memory for the generator with type T
       gsl_rng_set(r,seed);


      string base=make_base( value[9], n, Boxlength, zin);
      cout<< "Writing output to "<< (base+ ".*").c_str() <<endl;

      parout.open((base+ ".params").c_str());
      int j;
      //TODO adjust this to write out everything (automatically, instead of setting jmax by hand)
      for(j=0;j<9;j++){parout<< flag[j]<< "	" <<value[j]<<endl;}
      parout.close();

     MyFloat ain=1./(MyFloat)(zin+1);
      int quoppas=600; //max. lines in camb power spectrum file
      int c=7; //for transfer function

      double *inarr=(double*)calloc(quoppas*c,sizeof(double));
      double *kcamb=(double*)calloc(quoppas,sizeof(double));
      double *Tcamb=(double*)calloc(quoppas,sizeof(double));

      if(out!=0 && out!=1 && out!=2){cerr<< "Wrong output format, choose 0 (HDF5), 1 (Gadget) or 2 (both)"<<endl; return -1;}

#ifndef HAVE_HDF5
  if(out!=1){cerr<< "Not compiled with HDF5. Only output=1 is allowed in this case!"<<endl; return -1;}
#endif

      cout<< "Reading transfer file "<< value[8] <<"..."<<endl;

	GetBuffer(inarr, value[8], quoppas*c);

      MyFloat ap=inarr[1]; //to normalise CAMB transfer function so T(0)= 1, doesn't matter if we normalise here in terms of accuracy, but feels more natural

      int quoppa=0;

      for(j=0;j<quoppas;j++){
	if(inarr[c*j]>0){kcamb[j]=MyFloat(inarr[c*j]); Tcamb[j]=MyFloat(inarr[c*j+1])/MyFloat(ap); quoppa+=1;}
	else {continue;}
      }

	free(inarr);

       complex<MyFloat> *rnd=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
       complex<MyFloat> *ft=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));

       cout<< "Drawing random numbers..."<< endl;

       long i;
       MyFloat sigma=sqrt((MyFloat)(npartTotal));
       for(i=0;i<npartTotal;i++){rnd[i]=gsl_ran_gaussian_ziggurat(r,1.)*sigma;}// cout<< "rnd "<< rnd[i] << endl;}



       gsl_rng_free (r);


       cout<< "First FFT..." <<endl;

       ft=fft_r(ft, rnd, n, 1);

       std::cout<<"Initial chi^2 (white noise, real space) = " << dot(rnd,rnd,npartTotal)/((MyFloat) npartTotal) << std::endl;
       free(rnd);

       ft[0]=complex<MyFloat>(0.,0.); //assure mean==0


       std::cout<<"Initial chi^2 (white noise, fourier space) = " << dot(ft,ft,npartTotal)/((MyFloat) npartTotal) << std::endl;

      //scale white-noise delta with initial PS
      complex<MyFloat> *ftsc=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      int ix,iy,iz,idx;
      int iix, iiy, iiz;
      int res=n;
      MyFloat kfft;
      MyFloat grwfac;
      MyFloat ns=0.96; //TODO make this an input parameter (long term: run CAMB with input parameters to ensure consistency?)



      //growth factor normalised to 1 today:
      grwfac=D(ain, Om0, Ol0)/D(1., Om0, Ol0);

      cout<< "Growth factor " << grwfac << endl;
      MyFloat sg8;

      sg8=sig(8., kcamb, Tcamb, ns, Boxlength, n, quoppa);
      std::cout <<"Sigma_8 "<< sg8 << std::endl;

      MyFloat kw = 2.*M_PI/(MyFloat)Boxlength;
      MyFloat amp=(sigma8/sg8)*(sigma8/sg8)*grwfac*grwfac; //norm. for sigma8 and linear growth factor
      MyFloat norm=kw*kw*kw/powf(2.*M_PI,3.); //since kw=2pi/L, this is just 1/V_box

      complex<MyFloat> *P=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));


      std::cout<<"Interpolation: kmin: "<< kw <<" Mpc/h, kmax: "<< (MyFloat)( kw*res/2.*sqrt(3.)) <<" Mpc/h"<<std::endl;

       MyFloat norm_amp=norm*amp;
        brute_interpol_new(n, kcamb, Tcamb,  quoppa, kw, ns, norm_amp, ft, ftsc, P);

      //assert(abs(real(norm_iter)) >1e-12); //norm!=0
      //assert(abs(imag(norm_iter)) <1e-12); //because there shouldn't be an imaginary part since we assume d is purely real
      //TODO think about this (it fails more often than it should?)

      cout<<"Transfer applied!"<<endl;
      cout<< "Power spectrum sample: " << P[0] << " " << P[1] <<" " << P[npartTotal-1] <<endl;

      std::cout<<"Initial chi^2 (white noise, fourier space) = " << chi2(ftsc,P,npartTotal) << std::endl;

      complex<MyFloat> *ftsc_old=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      for(i=0;i<npartTotal;i++) {ftsc_old[i]=ftsc[i];}


      cout << "ftsc_old " << ftsc_old[0] << " " << ftsc_old[1] << " " << ftsc_old[2] << " " << ftsc_old[npartTotal-1] << endl;

//potential for use in alpha_mu constraint, based on unconstrained field
      cout<< "Calculating potential..."<<endl;

      complex<MyFloat>* potk0=(complex<MyFloat>*)calloc(n*n*n,sizeof(complex<MyFloat>));
      potk0=poiss(potk0, ftsc_old, n, Boxlength, ain, Om0); //pot in k-space

      complex<MyFloat>* pot0=(complex<MyFloat>*)calloc(n*n*n,sizeof(complex<MyFloat>));
      pot0=fft_r(pot0,potk0,res,-1); //pot in real-space


       Grid grid(n);
       MyFloat dx= Boxlength/n;
       // construct the description of the underlying field and its realization
       UnderlyingField<MyFloat> *pField = new UnderlyingField<MyFloat>(P, ftsc, npartTotal);
       MultiConstrainedField<MyFloat> constr(pField, npartTotal);

       MyFloat orig_chi2 = std::real(chi2(ftsc,P,npartTotal));
       std::cout<<"Initial chi^2 (scaled) = " << orig_chi2 << std::endl;

       while(!inf.eof()) {
           char command[100];
           command[0]=0;
           inf >> command;

           if(strcasecmp(command,"IDfile")==0) {
               char IDfile[100];
               inf >> IDfile;
               n_in_bin = AllocAndGetBuffer_int(IDfile, part_arr,0);
               cout << "New particle array " << IDfile << " loaded." << endl;
           } else if(strcasecmp(command,"append_IDfile")==0) {
               char IDfile[100];
               inf >> IDfile;
               n_in_bin = AllocAndGetBuffer_int(IDfile, part_arr, n_in_bin);
           } else if(strcasecmp(command,"order")==0) {
               reorderBuffer(part_arr, n_in_bin, npartTotal, res, dx, ain, Om0, Boxlength, &grid);
           } else if(strcasecmp(command,"truncate")==0) {
               float x;
               inf >> x;
               if(x<0 || x>1) {
                   cerr << "Truncate command takes a fraction between 0 and 1" << endl;
                   exit(0);
               }
               n_in_bin = ((int)(n_in_bin*x));
           }
           else if(strcasecmp(command,"calculate")==0) {
               complex<MyFloat> *vec = calcConstraintVector(inf, part_arr, n_in_bin, npartTotal, res, dx, ain, Om0, Boxlength, &grid);
               cout << "    --> calculated value = " << dot(vec, ftsc, npartTotal) << endl;
               free(vec);
           } else
	   if(strcasecmp(command,"constrain_direction")==0) {
	     // syntax: constrain_direction [and_renormalize] vec_name dir0 dir1 dir2 [renorm_fac]
	     std::stringstream ss;
	     char name[100];
	     bool normalization=false;
	     complex<MyFloat>* vecs[3];
	     complex<MyFloat> vals[3];
	     inf >> name;
	     if(strcasecmp(name,"and_renormalize")==0) {
	       normalization=true;
	       inf >> name;
	     }
	     for(int dir=0; dir<3; dir++) {
	       ss << name << " " ;
	       ss << dir << " " ;
	       vecs[dir] = calcConstraintVector(ss, part_arr, n_in_bin, npartTotal, res, dx, ain, Om0, Boxlength, &grid);
	       vals[dir] = dot(vecs[dir],ftsc,npartTotal);
	     }

	     complex<MyFloat> norm = std::sqrt(dot(vals,vals,3));
	     cerr << "   Initial values are " << vals[0] << " " << vals[1] << " " <<vals[2] << " -> norm = " << norm << endl;
	     MyFloat direction[3];
	     inf >> direction[0] >> direction[1] >> direction[2];
	     complex<MyFloat> in_norm = std::sqrt(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);

	     MyFloat costheta=0;
	     for(int dir=0; dir<3; dir++)
	       costheta+=direction[dir]*std::real(vals[dir])/std::real(norm*in_norm);

	     cerr << "   Between Re original and Re constrained, cos theta = " <<costheta << endl;

	     if(normalization) {
	       MyFloat renorm;
	       inf >> renorm;
	       norm*=renorm;
	     }

	     for(int dir=0; dir<3; dir++)
	       vals[dir]=direction[dir]*norm/in_norm;


	     cerr << "   Constrain values are " << vals[0] << " " << vals[1] << " " << vals[2] << endl;
	     for(int dir=0; dir<3; dir++)
	       constr.add_constraint(vecs[dir],vals[dir],vals[dir]*in_norm/norm);
	   } else
           if(strcasecmp(command,"constrain")==0) {
               bool relative=false;
               if(pField==NULL) {
                   cerr << "Eek! You're trying to add a constraint but the calculation is already done. Move your done command." << endl;
                   exit(0);
               }

               complex<MyFloat> *vec = calcConstraintVector(inf, part_arr, n_in_bin, npartTotal, res, dx, ain, Om0, Boxlength,  &grid);

               inf >> command;
               if (strcasecmp(command,"relative")==0) {
                   relative=true;
               } else if (strcasecmp(command,"absolute")!=0) {
                   cerr << "Constraints must state either relative or absolute" << endl;
                   exit(0);
               }

               MyFloat constraint_real;
               inf >> constraint_real;

               std::complex<MyFloat> constraint = constraint_real;
               std::complex<MyFloat> initv = dot(vec, ftsc, npartTotal);

               if(relative) constraint*=initv;

               cout << "    --> initial value = " << initv << ", constraining to " << constraint << endl;
	       constr.add_constraint(vec, constraint, initv);
           } else
           if(strcasecmp(command,"done")==0) {
               if(pField==NULL) {
                   cerr << "ERROR - field information is not present. Are there two 'done' commands perhaps?" << endl;
                   exit(0);
               }
               complex<MyFloat> *y1=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
	           constr.prepare();
               constr.get_realization(y1);

               for(long i=0; i<npartTotal; i++)
                   ftsc[i]=y1[i];

               free(y1);

               cout << "Expected Delta chi^2=" << constr.get_delta_chi2() << endl;

               // clean everything up
               delete pField;
               pField=NULL;

           }


       }

       if(pField!=NULL) {
           cerr << endl << endl << "WHOOPS - you didn't actually calculate the constraints. You need a 'done' command in the paramfile." << endl << endl;
       }


       inf.close();
       MyFloat final_chi2 = std::real(chi2(ftsc,P,npartTotal));
       std::cout<<"Final chi^2  = " <<  final_chi2 << std::endl;
       std::cout<<"Delta chi^2  = " <<  final_chi2 - orig_chi2 << std::endl;

      /*

      ConstrainedField<MyFloat> constrained_field_x  (&underlying_field,     alphak1_A, d*a1, npartTotal);
      ConstrainedField<MyFloat> constrained_field_xy (&constrained_field_x,  alphak2_A, d*a2, npartTotal);
      ConstrainedField<MyFloat> constrained_field_xyz(&constrained_field_xy, alphak3_A, d*a3, npartTotal);

      complex<MyFloat> *y1=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      constrained_field_xyz.get_realization(y1);

      // The following line isn't required - it's only here for debug information
      complex<MyFloat> *x1=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      constrained_field_xyz.get_mean(x1);

      */

      cout << endl;




   // ftsc now contains constrained field


   //output power spectrum of constrained field
   powsp_noJing(n, ftsc, (base+ ".ps").c_str(), Boxlength);


   //calculate potential of constrained field
   complex<MyFloat>* potk=(complex<MyFloat>*)calloc(n*n*n,sizeof(complex<MyFloat>));
   potk=poiss(potk, ftsc, n, Boxlength, ain, Om0); //pot in k-space

   complex<MyFloat>* pot=(complex<MyFloat>*)calloc(n*n*n,sizeof(complex<MyFloat>));
   pot=fft_r(pot,potk,res,-1); //pot in real-space



      free(ft);
      free(kcamb);
      free(Tcamb);
      free(P);


      complex<MyFloat>* psift1k=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      complex<MyFloat>* psift2k=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      complex<MyFloat>* psift3k=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      complex<MyFloat>* psift1=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      complex<MyFloat>* psift2=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      complex<MyFloat>* psift3=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));

      //complex<MyFloat>* test_arr=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));

      //Zeldovich approx.
      for(ix=0; ix<res;ix++){
	for(iy=0;iy<res;iy++){
	  for(iz=0;iz<res;iz++){
	    idx = (ix*res+iy)*(res)+iz;

	      //test_arr[idx].real(MyFloat(idx));
	      if( ix>res/2 ) iix = ix - res; else iix = ix;
	      if( iy>res/2 ) iiy = iy - res; else iiy = iy;
	      if( iz>res/2 ) iiz = iz - res; else iiz = iz;

	      kfft = sqrt(iix*iix+iiy*iiy+iiz*iiz);

	      psift1k[idx].real(-ftsc[idx].imag()/(MyFloat)(kfft*kfft)*iix/kw);
	      psift1k[idx].imag(ftsc[idx].real()/(MyFloat)(kfft*kfft)*iix/kw);
	      psift2k[idx].real(-ftsc[idx].imag()/(MyFloat)(kfft*kfft)*iiy/kw);
	      psift2k[idx].imag(ftsc[idx].real()/(MyFloat)(kfft*kfft)*iiy/kw);
	      psift3k[idx].real(-ftsc[idx].imag()/(MyFloat)(kfft*kfft)*iiz/kw);
	      psift3k[idx].imag(ftsc[idx].real()/(MyFloat)(kfft*kfft)*iiz/kw);
	  }
	}
      }



      complex<MyFloat> *delta_real=(complex<MyFloat>*)calloc(npartTotal,sizeof(complex<MyFloat>));
      delta_real=fft_r(delta_real,ftsc,res,-1);

      MyFloat *potr=(MyFloat*)calloc(n*n*n,sizeof(MyFloat));
      for(i=0;i<n*n*n;i++){potr[i]=(MyFloat)(pot[i].real()); pot[i].imag(0.);}

      //optional: save potential
      //string phases_out=(base+ "_phases.hdf5").c_str();
      //save_phases(potk, potr, delta_real, n, phases_out.c_str());

      MyFloat exp=0.;
      for(i=0;i<n*n*n;i++){exp+=(MyFloat)(pot[i].real()*pot[i].real());}
      exp/=(MyFloat)(n*n*n);

      free(pot);
      free(potk);
      free(potr);
      free(pot0);
      free(potk0);
      free(delta_real);

        free(ftsc_old);
        free(ftsc);

       psift1k[0]=complex<MyFloat>(0.,0.);
       psift2k[0]=complex<MyFloat>(0.,0.);
       psift3k[0]=complex<MyFloat>(0.,0.);

       psift1=fft_r(psift1,psift1k,n,-1); //the output .imag() part is non-zero because of the Nyquist frequency, but this is not used anywhere else
       psift2=fft_r(psift2,psift2k,n,-1); //same
       psift3=fft_r(psift3,psift3k,n,-1); //same

      free(psift1k);
      free(psift2k);
      free(psift3k);


       MyFloat gr=Boxlength/(MyFloat)n;
       cout<< "Grid cell size: "<< gr <<" Mpc/h"<<endl;

      MyFloat *Vel1=(MyFloat*)calloc(npartTotal,sizeof(MyFloat));
      MyFloat *Vel2=(MyFloat*)calloc(npartTotal,sizeof(MyFloat));
      MyFloat *Vel3=(MyFloat*)calloc(npartTotal,sizeof(MyFloat));
      MyFloat *Pos1=(MyFloat*)calloc(npartTotal,sizeof(MyFloat));
      MyFloat *Pos2=(MyFloat*)calloc(npartTotal,sizeof(MyFloat));
      MyFloat *Pos3=(MyFloat*)calloc(npartTotal,sizeof(MyFloat));

      MyFloat hfac=1.*100.*sqrt(Om0/ain/ain/ain+Ol0)*sqrt(ain); //this should be f*H(t)*a, but gadget wants vel/sqrt(a), so we use H(t)*sqrt(a) //TODO: hardcoded value of f=1 is inaccurate, but fomega currently gives wrong results

    MyFloat mean1=0.,mean2=0.,mean3=0.;
    MyFloat Mean1=0., Mean2=0., Mean3=0.;
    cout<< "Applying ZA & PBC... "<<endl;
      //apply ZA:
       for(ix=0;ix<res;ix++){
	 for(iy=0;iy<res;iy++){
	   for(iz=0;iz<res;iz++){
	    idx = (ix*res+iy)*(res)+iz;

	    Vel1[idx] = psift1[idx].real()*hfac; //physical units
	    Vel2[idx] = psift2[idx].real()*hfac;
	    Vel3[idx] = psift3[idx].real()*hfac;

	    //position in "grid coordinates": Pos e [0,N-1]
	    Pos1[idx] = psift1[idx].real()*n/Boxlength+ix;
	    Pos2[idx] = psift2[idx].real()*n/Boxlength+iy;
	    Pos3[idx] = psift3[idx].real()*n/Boxlength+iz;

	    mean1+=abs(psift1[idx].real()*n/Boxlength);
	    mean2+=abs(psift2[idx].real()*n/Boxlength);
	    mean3+=abs(psift3[idx].real()*n/Boxlength);

	    //enforce periodic boundary conditions
	    if(Pos1[idx]<0.){Pos1[idx]+=(MyFloat)res;}
	    else if(Pos1[idx]>(MyFloat)res){Pos1[idx]-=(MyFloat)res;}
	    if(Pos2[idx]<0.){Pos2[idx]+=(MyFloat)res;}
	    else if(Pos2[idx]>(MyFloat)res){Pos2[idx]-=(MyFloat)res;}
	    if(Pos3[idx]<0.){Pos3[idx]+=(MyFloat)res;}
	    else if(Pos3[idx]>(MyFloat)res){Pos3[idx]-=(MyFloat)res;}

	    //rescale to physical coordinates
	    Pos1[idx] *= (Boxlength/(MyFloat)n);
	    Pos2[idx] *= (Boxlength/(MyFloat)n);
	    Pos3[idx] *= (Boxlength/(MyFloat)n);

	    Mean1+=Pos1[idx];
	    Mean2+=Pos2[idx];
	    Mean3+=Pos3[idx];

	   }
	 }
       }


      cout<< "Box/2="<< Boxlength/2.<< " Mpc/h, Mean position x,y,z: "<< Mean1/(MyFloat(res*res*res))<<" "<< Mean2/(MyFloat(res*res*res))<<" "<<Mean3/(MyFloat(res*res*res))<< " Mpc/h"<<  endl;

        free(psift1);
        free(psift2);
        free(psift3);

	MyFloat pmass=27.78*Om0*powf(Boxlength/(MyFloat)(res),3.0); // in 10^10 M_sol
	cout<< "Particle mass: " <<pmass <<" [10^10 M_sun]"<<endl;


   if (gadgetformat==2){
	header2.npart[0]=0;
	header2.npart[1]=npartTotal;
	header2.npart[2]=0;
	header2.npart[3]=0;
	header2.npart[4]=0;
	header2.npart[5]=0;
	header2.mass[0]=0;
	header2.mass[1]=pmass;
	header2.mass[2]=0;
	header2.mass[3]=0;
	header2.mass[4]=0;
	header2.mass[5]=0;
	header2.time=ain;
	header2.redshift=zin;
	header2.flag_sfr=0;
	header2.flag_feedback=0;
	header2.npartTotal[0]=0;
	header2.npartTotal[1]=npartTotal;
	header2.npartTotal[2]=0;
	header2.npartTotal[3]=0;
	header2.npartTotal[4]=0;
	header2.npartTotal[5]=0;
	header2.flag_cooling=0;
	header2.num_files=1;
	header2.BoxSize=Boxlength;
	header2.Omega0=Om0;
	header2.OmegaLambda=Ol0;
 	header2.HubbleParam=0.701;
     }

    if (gadgetformat==3){
	header3.npart[0]=0;
        header3.npart[1]=(unsigned int)(npartTotal);
        header3.npart[2]=0;
        header3.npart[3]=0;
        header3.npart[4]=0;
        header3.npart[5]=0;
        header3.mass[0]=0;
        header3.mass[1]=pmass;
        header3.mass[2]=0;
        header3.mass[3]=0;
        header3.mass[4]=0;
        header3.mass[5]=0;
        header3.time=ain;
        header3.redshift=zin;
        header3.flag_sfr=0;
        header3.flag_feedback=0;
        header3.npartTotal[0]=0;
        header3.npartTotal[1]=(unsigned int)(npartTotal);
        header3.npartTotal[2]=0;
        header3.npartTotal[3]=0;
        header3.npartTotal[4]=0;
        header3.npartTotal[5]=0;
        header3.flag_cooling=0;
        header3.num_files=1;
        header3.BoxSize=Boxlength;
        header3.Omega0=Om0;
        header3.OmegaLambda=Ol0;
        header3.HubbleParam=0.701;
	header3.flag_stellarage=0;	/*!< flags whether the file contains formation times of star particles */
  	header3.flag_metals=0;		/*!< flags whether the file contains metallicity values for gas and star  particles */
  	header3.npartTotalHighWord[0]=0;
	header3.npartTotalHighWord[1]=(unsigned int) (npartTotal >> 32); //copied from Gadget3
	header3.npartTotalHighWord[2]=0;
	header3.npartTotalHighWord[3]=0;
	header3.npartTotalHighWord[4]=0;
	header3.npartTotalHighWord[5]=0;
  	header3.flag_entropy_instead_u=0;	/*!< flags that IC-file contains entropy instead of u */
#ifdef OUTPUT_IN_DOUBLEPRECISION
	header3.flag_doubleprecision=1; /*!< flags that snapshot contains double-precision instead of single precision */
#else
        header3.flag_doubleprecision=0;
#endif
  	header3.flag_ic_info=1;
     }

	string dname="Coordinates";
	string dname2="Velocities";
	string dname3="ParticleIDs";


	if(out==0){
#ifdef HAVE_HDF5
	    SaveHDF( (base+ ".hdf5").c_str(), n, header3, Pos1, Pos2, Pos3, Vel1,  Vel2,  Vel3, dname.c_str(), dname2.c_str(), dname3.c_str());
#endif
	}

	else if(out==1){
	    if (gadgetformat==2) SaveGadget2( (base+ "_gadget2.dat").c_str(), n, header2, Pos1, Vel1, Pos2, Vel2, Pos3, Vel3);
	    if (gadgetformat==3) SaveGadget3( (base+ "_gadget3.dat").c_str(), n, header3, Pos1, Vel1, Pos2, Vel2, Pos3, Vel3);
	}

	else{
	    if (gadgetformat==2) SaveGadget2( (base+ "_gadget2.dat").c_str(), n, header2, Pos1, Vel1, Pos2, Vel2, Pos3, Vel3);
	    if (gadgetformat==3) SaveGadget3( (base+ "_gadget3.dat").c_str(), n, header3, Pos1, Vel1, Pos2, Vel2, Pos3, Vel3);
#ifdef HAVE_HDF5
	    SaveHDF( (base+ ".hdf5").c_str(), n, header3, Pos1, Pos2, Pos3, Vel1,  Vel2,  Vel3,  dname.c_str(), dname2.c_str(), dname3.c_str());
#endif
	}

	free(Pos1);
	free(Vel1);
	free(Pos2);
	free(Vel2);
	free(Pos3);
	free(Vel3);

       cout<<"Done!"<<endl;

       return 0;
}
