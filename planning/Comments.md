Comments
Judiciously placed comments in the code can provide information that a person could
not discern simply by reading the code. Comments can be added at many different
levels.
• At the program level, you can include a README file that provides a general
description of the program and explains its organization.
• At the file level, it is good practice to include a file prolog that explains the
purpose of the file and provides other information (discussed in more detail in
Section 4).
• At the function level, a comment can serve as a function prolog.
• Throughout the file, where data are being declared or defined, it is helpful to
add comments to explain the purpose of the variables.
Comments can be written in several styles depending on their purpose and length.
Use comments to add information for the reader or to highlight sections of code.
Do not paraphrase the code or repeat information contained in the Program Design
Language (PDL).

This section describes the use of comments and provides examples.
• Boxed comments—Use for prologs or as section separators
• Block comments—Use at the beginning of each major section of the code as a
narrative description of that portion of the code.
• Short comments—Write on the same line as the code or data definition they
describe.
• Inline comments—Write at the same level of indentation as the code they
describe.
Example: boxed comment prolog
/*****************************************************
 * FILE NAME *
 * *
 * PURPOSE *
 * *
 *****************************************************/
Example: section separator
/*****************************************************/
Example: block comment
/*
 * Write the comment text here, in complete sentences.
 * Use block comments when there is more than one
 * sentence.
 */
Example: short comments
double ieee_r[]; /* array of IEEE real*8 values */
unsigned char ibm_r[]; /* string of IBM real*8 values */
int count; /* number of real*8 values */
• Tab comment over far enough to separate it from code statements.
• If more than one short comment appears in a block of code or data
definition, start all of them at the same tab position and end all at the same
position.
Example: inline comment
switch (ref_type)
{
/* Perform case for either s/c position or velocity
 * vector request using the RSL routine c_calpvs */
case 1:
case 2:
...
case n:
}
In general, use short comments to document variable definitions and block comments
to describe computation processes.
Example: block comment vs. short comment
preferred style:
/*
 * Main sequence: get and process all user requests
 */
while (!finish())
{
inquire();
process();
}
not recommended:
while (!finish()) /* Main sequence: */
{ /* */
inquire(); /* Get user request */
process(); /* And carry it out */
} /* As long as possible */