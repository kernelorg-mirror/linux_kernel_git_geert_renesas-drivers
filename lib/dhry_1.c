/*
 ****************************************************************************
 *
 *                   "DHRYSTONE" Benchmark Program
 *                   -----------------------------
 *
 *  Version:    C, Version 2.1
 *
 *  File:       dhry_1.c (part 2 of 3)
 *
 *  Date:       May 25, 1988
 *
 *  Author:     Reinhold P. Weicker
 *
 ****************************************************************************
 */

#include "dhry.h"

#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/string.h>

/* Global Variables: */

Rec_Pointer     Ptr_Glob,
                Next_Ptr_Glob;
int             Int_Glob;
Boolean         Bool_Glob;
char            Ch_1_Glob,
                Ch_2_Glob;
int             Arr_1_Glob [50];
int             Arr_2_Glob [50] [50];

Enumeration     Func_1 (Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val);
  /* forward declaration necessary since Enumeration may not simply be int */

/* variables for time measurement: */

#define Too_Small_Time (2 * MSEC_PER_SEC)
                /* Measurements should last at least 2 seconds */

ktime_t         Begin_Time,
                End_Time;
u32             User_Time;
u64             Dhrystones_Per_Second;

/* end of variables for time measurement */


void dhry (int n)
/*****/

  /* main program, corresponds to procedures        */
  /* Main and Proc_0 in the Ada version             */
{
        One_Fifty       Int_1_Loc;
        One_Fifty       Int_2_Loc;
        One_Fifty       Int_3_Loc;
        char            Ch_Index;
        Enumeration     Enum_Loc;
        Str_30          Str_1_Loc;
        Str_30          Str_2_Loc;
        int             Run_Index;
        int             Number_Of_Runs;

  /* Initializations */

  Next_Ptr_Glob = (Rec_Pointer) kzalloc (sizeof (Rec_Type), GFP_KERNEL);
  Ptr_Glob = (Rec_Pointer) kzalloc (sizeof (Rec_Type), GFP_KERNEL);

  Ptr_Glob->Ptr_Comp                    = Next_Ptr_Glob;
  Ptr_Glob->Discr                       = Ident_1;
  Ptr_Glob->variant.var_1.Enum_Comp     = Ident_3;
  Ptr_Glob->variant.var_1.Int_Comp      = 40;
  strcpy (Ptr_Glob->variant.var_1.Str_Comp,
          "DHRYSTONE PROGRAM, SOME STRING");
  strcpy (Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");

  Arr_2_Glob [8][7] = 10;
        /* Was missing in published program. Without this statement,    */
        /* Arr_2_Glob [8][7] would have an undefined value.             */
        /* Warning: With 16-Bit processors and Number_Of_Runs > 32000,  */
        /* overflow may occur for this array element.                   */

  pr_info ("\n");
  pr_info ("Dhrystone Benchmark, Version 2.1 (Language: C)\n");
  pr_info ("\n");

  Number_Of_Runs = n;

  pr_info ("Execution starts, %d runs through Dhrystone\n", Number_Of_Runs);

  /***************/
  /* Start timer */
  /***************/

  Begin_Time = ktime_get();

  for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index)
  {

    Proc_5();
    Proc_4();
      /* Ch_1_Glob == 'A', Ch_2_Glob == 'B', Bool_Glob == true */
    Int_1_Loc = 2;
    Int_2_Loc = 3;
    strcpy (Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
    Enum_Loc = Ident_2;
    Bool_Glob = ! Func_2 (Str_1_Loc, Str_2_Loc);
      /* Bool_Glob == 1 */
    while (Int_1_Loc < Int_2_Loc)  /* loop body executed once */
    {
      Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
        /* Int_3_Loc == 7 */
      Proc_7 (Int_1_Loc, Int_2_Loc, &Int_3_Loc);
        /* Int_3_Loc == 7 */
      Int_1_Loc += 1;
    } /* while */
      /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
    Proc_8 (Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
      /* Int_Glob == 5 */
    Proc_1 (Ptr_Glob);
    for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index)
                             /* loop body executed twice */
    {
      if (Enum_Loc == Func_1 (Ch_Index, 'C'))
          /* then, not executed */
        {
        Proc_6 (Ident_1, &Enum_Loc);
        strcpy (Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
        Int_2_Loc = Run_Index;
        Int_Glob = Run_Index;
        }
    }
      /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
    Int_2_Loc = Int_2_Loc * Int_1_Loc;
    Int_1_Loc = Int_2_Loc / Int_3_Loc;
    Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
      /* Int_1_Loc == 1, Int_2_Loc == 13, Int_3_Loc == 7 */
    Proc_2 (&Int_1_Loc);
      /* Int_1_Loc == 5 */

  } /* loop "for Run_Index" */

  /**************/
  /* Stop timer */
  /**************/

  End_Time = ktime_get();

  pr_info ("Execution ends\n");
  pr_info ("\n");
  pr_info ("Final values of the variables used in the benchmark:\n");
  pr_info ("\n");
  pr_info ("Int_Glob:            %d\n", Int_Glob);
  pr_info ("        should be:   %d\n", 5);
  pr_info ("Bool_Glob:           %d\n", Bool_Glob);
  pr_info ("        should be:   %d\n", 1);
  pr_info ("Ch_1_Glob:           %c\n", Ch_1_Glob);
  pr_info ("        should be:   %c\n", 'A');
  pr_info ("Ch_2_Glob:           %c\n", Ch_2_Glob);
  pr_info ("        should be:   %c\n", 'B');
  pr_info ("Arr_1_Glob[8]:       %d\n", Arr_1_Glob[8]);
  pr_info ("        should be:   %d\n", 7);
  pr_info ("Arr_2_Glob[8][7]:    %d\n", Arr_2_Glob[8][7]);
  pr_info ("        should be:   Number_Of_Runs + 10\n");
  pr_info ("Ptr_Glob->\n");
  pr_info ("  Ptr_Comp:          %d\n", (int) Ptr_Glob->Ptr_Comp);
  pr_info ("        should be:   (implementation-dependent)\n");
  pr_info ("  Discr:             %d\n", Ptr_Glob->Discr);
  pr_info ("        should be:   %d\n", 0);
  pr_info ("  Enum_Comp:         %d\n", Ptr_Glob->variant.var_1.Enum_Comp);
  pr_info ("        should be:   %d\n", 2);
  pr_info ("  Int_Comp:          %d\n", Ptr_Glob->variant.var_1.Int_Comp);
  pr_info ("        should be:   %d\n", 17);
  pr_info ("  Str_Comp:          %s\n", Ptr_Glob->variant.var_1.Str_Comp);
  pr_info ("        should be:   DHRYSTONE PROGRAM, SOME STRING\n");
  pr_info ("Next_Ptr_Glob->\n");
  pr_info ("  Ptr_Comp:          %d\n", (int) Next_Ptr_Glob->Ptr_Comp);
  pr_info ("        should be:   (implementation-dependent), same as above\n");
  pr_info ("  Discr:             %d\n", Next_Ptr_Glob->Discr);
  pr_info ("        should be:   %d\n", 0);
  pr_info ("  Enum_Comp:         %d\n", Next_Ptr_Glob->variant.var_1.Enum_Comp);
  pr_info ("        should be:   %d\n", 1);
  pr_info ("  Int_Comp:          %d\n", Next_Ptr_Glob->variant.var_1.Int_Comp);
  pr_info ("        should be:   %d\n", 18);
  pr_info ("  Str_Comp:          %s\n",
                                Next_Ptr_Glob->variant.var_1.Str_Comp);
  pr_info ("        should be:   DHRYSTONE PROGRAM, SOME STRING\n");
  pr_info ("Int_1_Loc:           %d\n", Int_1_Loc);
  pr_info ("        should be:   %d\n", 5);
  pr_info ("Int_2_Loc:           %d\n", Int_2_Loc);
  pr_info ("        should be:   %d\n", 13);
  pr_info ("Int_3_Loc:           %d\n", Int_3_Loc);
  pr_info ("        should be:   %d\n", 7);
  pr_info ("Enum_Loc:            %d\n", Enum_Loc);
  pr_info ("        should be:   %d\n", 1);
  pr_info ("Str_1_Loc:           %s\n", Str_1_Loc);
  pr_info ("        should be:   DHRYSTONE PROGRAM, 1'ST STRING\n");
  pr_info ("Str_2_Loc:           %s\n", Str_2_Loc);
  pr_info ("        should be:   DHRYSTONE PROGRAM, 2'ND STRING\n");
  pr_info ("\n");

  User_Time = ktime_to_ms(ktime_sub(End_Time, Begin_Time));

  if (User_Time < Too_Small_Time)
  {
    pr_info ("Measured time too small to obtain meaningful results\n");
    pr_info ("Please increase number of runs\n");
    pr_info ("\n");
  }
  else
  {
    Dhrystones_Per_Second = div_u64((u64)MSEC_PER_SEC * Number_Of_Runs,
                                    User_Time);
    pr_info ("Dhrystones per Second:                      ");
    pr_info ("%llu\n", Dhrystones_Per_Second);
    pr_info ("\n");
  }

  kfree(Ptr_Glob);
  kfree(Next_Ptr_Glob);
}


void Proc_1 (Rec_Pointer Ptr_Val_Par)
/******************/
    /* executed once */
{
  Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
                                        /* == Ptr_Glob_Next */
  /* Local variable, initialized with Ptr_Val_Par->Ptr_Comp,    */
  /* corresponds to "rename" in Ada, "with" in Pascal           */

  *Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
  Ptr_Val_Par->variant.var_1.Int_Comp = 5;
  Next_Record->variant.var_1.Int_Comp
        = Ptr_Val_Par->variant.var_1.Int_Comp;
  Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
  Proc_3 (&Next_Record->Ptr_Comp);
    /* Ptr_Val_Par->Ptr_Comp->Ptr_Comp
                        == Ptr_Glob->Ptr_Comp */
  if (Next_Record->Discr == Ident_1)
    /* then, executed */
  {
    Next_Record->variant.var_1.Int_Comp = 6;
    Proc_6 (Ptr_Val_Par->variant.var_1.Enum_Comp,
           &Next_Record->variant.var_1.Enum_Comp);
    Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
    Proc_7 (Next_Record->variant.var_1.Int_Comp, 10,
           &Next_Record->variant.var_1.Int_Comp);
  }
  else /* not executed */
    *Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
} /* Proc_1 */


void Proc_2 (One_Fifty *Int_Par_Ref)
/******************/
    /* executed once */
    /* *Int_Par_Ref == 1, becomes 4 */
{
  One_Fifty  Int_Loc;
  Enumeration   Enum_Loc;

  Int_Loc = *Int_Par_Ref + 10;
  do /* executed once */
    if (Ch_1_Glob == 'A')
      /* then, executed */
    {
      Int_Loc -= 1;
      *Int_Par_Ref = Int_Loc - Int_Glob;
      Enum_Loc = Ident_1;
    } /* if */
  while (Enum_Loc != Ident_1); /* true */
} /* Proc_2 */


void Proc_3 (Rec_Pointer *Ptr_Ref_Par)
/******************/
    /* executed once */
    /* Ptr_Ref_Par becomes Ptr_Glob */
{
  if (Ptr_Glob)
    /* then, executed */
    *Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
  Proc_7 (10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
} /* Proc_3 */


void Proc_4 (void)
/*******/
    /* executed once */
{
  Boolean Bool_Loc;

  Bool_Loc = Ch_1_Glob == 'A';
  Bool_Glob = Bool_Loc | Bool_Glob;
  Ch_2_Glob = 'B';
} /* Proc_4 */


void Proc_5 (void)
/*******/
    /* executed once */
{
  Ch_1_Glob = 'A';
  Bool_Glob = false;
} /* Proc_5 */
