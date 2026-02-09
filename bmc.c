#include<time.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>

/*
This program performs a binary search to find the path with the most number of memory instructions by performing multiple runs of the Z3 solver.
Each run results in an output file smt_model.txt with either SAT or UNSAT (and a model in case of SAT). 
The program takes as input an interval [l,h] and an smt file (file.smt): e.g. ./bmc 0 30 smt_lib2_add_ref.smt and returns the maximum memory access count as well as the corresponding path in smt_model.txt.
 
*/



/* AddMemConstraint() takes the name of an SMT file and adds a property "(assert (>= mem_cost m))" as a constraint to be verified at the end of the file */
FILE* AddMemConstraint(char* smtFile, int m)
{
  //char maccess[sizeof(m)];
  char maccess[1000];
  //char assert[22] = "(assert (>= mem_cost ";
  char assert[2000] = "(assert (>= mem_cost ";
  char checksat[50] = ")) \n(check-sat)\n(get-model)\n";   
  char copyToSMTf[100] = "";
  FILE *fp, *fp1;
  char* smtf = malloc( sizeof(char) * (strlen(smtFile)+1));
  strcpy(smtf, smtFile);
  strcat(copyToSMTf, "cp ");
  strcat(copyToSMTf, smtf);
  strcat(copyToSMTf, " tempf.smt");
  //printf("command line is: %s\n", copyToSMTf);
  //fp = popen("cp smt_lib2_add_ref.smt tempf.smt","w");
  fp = popen(copyToSMTf ,"w"); //This is equivalent to: fp = popen("cp smt_lib2_add_ref.smt tempf.smt","w"). 
  pclose(fp);
  fp1 = fopen("tempf.smt", "a");
  if(fp1==NULL) 
    perror("Error opening file.");
  else
  {
    sprintf(maccess, "%d", m);
    //printf("bmc.c: maccess=%s\n", maccess);
    strcat(assert, maccess);
    //printf("bmc.c: assert=%s\n", assert);
    strcat(assert, checksat);
    //printf("bmc.c: assert=%s\n", assert);
    fputs(assert,fp1);
  }
  //fclose(smtFile);
  fclose(fp1);
  return fp1;
}





int main(int argc, char *argv[])
{
  FILE *fp, *z3Output, *fp1, *tempf;
  int status;
  char sat[8];
  int l = 0;
  int h = 0;
  int m = 0;
  int temp_m=m;
  char* arg3;
  clock_t tic, toc;
  //Parsing and verifying command line arguments.
  if(argc==4){
    arg3 = malloc( sizeof(char) * (strlen(argv[3])+1) );;
    strcpy(arg3, argv[3]);
    fp1 = fopen(arg3, "r");
    if (fp1 == NULL){
      printf("Input File name is incorrect or file does not exist. Please enter a valid file name as a third argument: e.g. ./bmc 0 30 smt_lib2_add_ref.smt\n");
      return 0;
    }
    else{
      fclose(fp1);
      printf("Input SMT file is : %s\n", arg3);
    }
    l = atoi(argv[1]); // Using atoi() to convert a char to an int
    //printf("Initial value of l is: %d\n", l);
    h = atoi(argv[2]); // Using atoi() to convert a char to an int
    //printf("Initial value of h is: %d\n", h);
    printf("Initial search interval is [%d, %d]\n\n",l,h);
    if(l>=h){
      printf("Make sure your input arguments are correct: the first argument should be the lower bound of the interval and the second argument should be the higher bound of the interval. e.g. ./bmc 0 30 smt_lib2_add_ref.smt\n");
      return 0;
    }
  }
  else{
    printf("Command line arguments are incorrect or missing. Please enter 3 arguments (low bound, high bound, file name): e.g. ./bmc 0 30 smt_lib2_add_ref.smt\n");
    return 0;
  }
  

  tic = clock();
 //Binary search ends when the interval [l,h] is reduced to a singleton. 
  while(l != h){
    
    m=(l+h)/2;
    tempf = AddMemConstraint(arg3, m);
    fp = popen("z3 -smt2  tempf.smt > smt_model.txt","w");
    //printf("iteration\n");
    if (fp == NULL)
      printf("file popening error: make sure your input arguments are correct (./bmc l h file_name.smt) \n");

    status = pclose(fp);
    if (status == -1) 
      printf("file pclosing error\n");

    z3Output = fopen("smt_model.txt", "r");
    if (z3Output == NULL)
      printf("file opening error: make sure your input arguments are correct (./bmc l h file_name.smt)\n");
    else{
      fgets(sat,8,z3Output);
      fclose(z3Output);
      if ((strcmp(sat, "sat\n"))==0){
        int temp_l = l;
        l = m;
        if(l == temp_l) //if l stayed the same as in the previous iterarion then increment it (this is due to the output of the division being of type int). 
          l = m + 1;
        printf("verifying m >= %d, %s           => New search interval = [%d, %d].\n",m,sat,l,h);
      }  
      else if ((strcmp(sat, "unsat\n"))==0){
        int temp_h = h;
        h = m;
        if(h == temp_h) //if h stayed the same as in the previous iterarion then increment it (this is due to the output of the division being of type int).
         h = m + 1;
        printf("verifying m >= %d, %s           => New search interval = [%d, %d].\n",m,sat,l,h);
     }
      else 
      { 
        printf("error in creating or opening smt_model.txt: make sure your input arguments are correct (./bmc l h file_name.smt)\n");
        break;
     }
    }
  }
  toc = clock();
  if((strcmp(sat, "sat\n"))==0)  
    printf("The maximum memory access count is:%d and the corresponding path could be found in file smt_model.txt\n",m);
  if((strcmp(sat, "unsat\n"))==0)  
    printf("Property unsatisfiable: consider changing your search interval\n");
  printf("Search time is: %f seconds\n", (double)(toc - tic) / CLOCKS_PER_SEC);
  return m;
}

