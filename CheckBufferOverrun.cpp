
//---------------------------------------------------------------------------
// Buffer overrun..
//---------------------------------------------------------------------------

#include "CheckBufferOverrun.h"
#include "tokenize.h"
#include "CommonCheck.h"

#include <sstream>
#include <list>

#include <stdlib.h>     // <- strtoul

//---------------------------------------------------------------------------
extern bool ShowAll;
//---------------------------------------------------------------------------

// CallStack used when parsing into subfunctions.
static std::list<const TOKEN *> CallStack;

// Modified version of 'ReportError' that also reports the callstack
static void ReportError(const TOKEN *tok, const char errmsg[])
{
    std::ostringstream ostr;
    std::list<const TOKEN *>::const_iterator it;
    for ( it = CallStack.begin(); it != CallStack.end(); it++ )
        ostr << FileLine(*it) << " -> ";
    ostr << FileLine(tok) << ": " << errmsg;
    ReportErr(ostr.str());
}



//---------------------------------------------------------------------------
// Checking local variables in a scope
//---------------------------------------------------------------------------

static void CheckBufferOverrun_LocalVariable_CheckScope( const TOKEN *tok, const char varname[], const int size, const int total_size )
{
    int indentlevel = 0;
    for ( ; tok; tok = tok->next )
    {
        if (tok->str[0]=='{')
        {
            indentlevel++;
            continue;
        }

        if (tok->str[0]=='}')
        {
            indentlevel--;
            if ( indentlevel < 0 )
                return;
            continue;
        }


        // Array index..
        if (strcmp(tok->str,varname)==0 && match(tok->next,"[ num ]"))
        {
            const char *str = getstr(tok, 2);
            if (strtol(str, NULL, 10) >= size)
            {
                ReportError(tok, "Array index out of bounds");
            }
            continue;
        }


        // memset, memcmp, memcpy, strncpy, fgets..
        if (strcmp(tok->str,"memset")==0 ||
            strcmp(tok->str,"memcpy")==0 ||
            strcmp(tok->str,"memmove")==0 ||
            strcmp(tok->str,"memcmp")==0 ||
            strcmp(tok->str,"strncpy")==0 ||
            strcmp(tok->str,"fgets")==0 )
        {
            if (match(tok->next,"( var , num , num )") ||
                match(tok->next,"( var , var , num )") )
            {
                const char *var1 = getstr(tok, 2);
                const char *var2 = getstr(tok, 4);
                const char *num  = getstr(tok, 6);

                if ( atoi(num)>total_size &&
                     (strcmp(var1,varname)==0 ||
                      strcmp(var2,varname)==0 ) )
                {
                    ReportError(tok, "Buffer overrun");
                }
            }
            continue;
        }


        // Loop..
        if ( match(tok, "for (") )
        {
            const TOKEN *tok2 = gettok( tok, 2 );

            // for - setup..
            if ( match(tok2, "var = 0 ;") )
                tok2 = gettok(tok2, 4);
            else if ( match(tok2, "type var = 0 ;") )
                tok2 = gettok(tok2, 5);
            else if ( match(tok2, "type type var = 0 ;") )
                tok2 = gettok(tok2, 6);
            else
                continue;

            // for - condition..
            if ( ! match(tok2, "var < num ;") && ! match(tok2, "var <= num ;"))
                continue;

            // Get index variable and stopsize.
            const char *strindex = tok2->str;
            int value = (tok2->next->str[1] ? 1 : 0) + atoi(getstr(tok2, 2));
            if ( value <= size )
                continue;

            // Goto the end of the for loop..
            while (tok2 && strcmp(tok2->str,")"))
                tok2 = tok2->next;
            if (!gettok(tok2,5))
                break;
            int indentlevel2 = 0;
            while (tok2)
            {
                if ( tok2->str[0] == ';' && indentlevel == 0 )
                    break;

                if ( tok2->str[0] == '{' )
                    indentlevel2++;

                if ( tok2->str[0] == '}' )
                {
                    indentlevel2--;
                    if ( indentlevel2 <= 0 )
                        break;
                }

                if ( match( tok2, "var [ var ]" ) &&
                     strcmp(tok2->str,varname)==0 &&
                     strcmp(getstr(tok2,2),strindex)==0 )
                {
                    ReportError(tok2, "Buffer overrun");
                    break;
                }

                tok2 = tok2->next;
            }
            continue;
        }


        // Writing data into array..
        if (match(tok,"strcpy ( var , "))
        {
            int len = 0;
            if (strcmp(getstr(tok, 2), varname) == 0)
            {
                const char *str = getstr(tok, 4);
                if (str[0] == '\"')
                {
                    while (*str)
                    {
                        if (*str=='\\')
                            str++;
                        str++;
                        len++;
                    }
                }
            }
            if (len > 2 && len >= (int)size + 2)
            {
                ReportError(tok, "Buffer overrun");
            }
            continue;
        }


        // Function call..
        if ( match( tok, "var (" ) )
        {
            // Don't make recursive checking..
            if (std::find(CallStack.begin(), CallStack.end(), tok) != CallStack.end())
                continue;

            unsigned int parlevel = 0, par = 0;
            for ( const TOKEN *tok2 = tok; tok2; tok2 = tok2->next )
            {
                if ( tok2->str[0] == '(' )
                {
                    parlevel++;
                }

                else if ( tok2->str[0] == ')' )
                {
                    parlevel--;
                    if ( parlevel < 1 )
                    {
                        par = 0;
                        break;
                    }
                }

                else if ( parlevel == 1 && tok2->str[0] == ',' )
                {
                    par++;
                }

                if ( parlevel == 1 &&
                    strchr( "(,", *getstr(tok2,0) ) &&
                    strcmp( varname, getstr(tok2, 1) ) == 0 &&
                    strchr( ",)", *getstr(tok2,2) ) )
                {
                    par++;
                    break;
                }
            }

            if ( par == 0 )
                continue;

            // Find function..
            const TOKEN *ftok = FindFunction( tokens, tok->str );
            if ( ! ftok )
                continue;

            // Parse head of function..
            ftok = gettok( ftok, 2 );
            parlevel = 1;
            while ( ftok && parlevel == 1 && par >= 1 )
            {
                if ( ftok->str[0] == '(' )
                    parlevel++;

                else if ( ftok->str[0] == ')' )
                    parlevel--;

                else if ( ftok->str[0] == ',' )
                    par--;

                else if (par==1 && parlevel==1 && (match(ftok,"var ,") || match(ftok,"var )")))
                {
                    // Parameter name..
                    const char *parname = ftok->str;

                    // Goto function body..
                    while ( ftok && ftok->str[0] != '{' )
                        ftok = ftok->next;
                    ftok = ftok ? ftok->next : 0;

                    // Check variable usage in the function..
                    CallStack.push_back( tok );
                    CheckBufferOverrun_LocalVariable_CheckScope( ftok, parname, size, total_size );
                    CallStack.pop_back();

                    // break out..
                    break;
                }

                ftok = ftok->next;
            }
        }
    }
}


static void CheckBufferOverrun_LocalVariable()
{
    int indentlevel = 0;
    for (const TOKEN *tok = tokens; tok; tok = tok->next)
    {
        if (tok->str[0]=='{')
            indentlevel++;

        else if (tok->str[0]=='}')
            indentlevel--;

        else if (indentlevel > 0 && match(tok, "type var [ num ] ;"))
        {
            const char *varname = getstr(tok,1);
            unsigned int size = strtoul(getstr(tok,3), NULL, 10);
            int total_size = size * SizeOfType(tok->str);
            if (total_size == 0)
                continue;

            // The callstack is empty
            CallStack.clear();
            CheckBufferOverrun_LocalVariable_CheckScope( gettok(tok,5), varname, size, total_size );
        }
    }
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Checking member variables of structs..
//---------------------------------------------------------------------------

static void CheckBufferOverrun_StructVariable_CheckVar( const TOKEN *tok1, const char varname[], const char arrname[], const int arrsize )
{
    const char *badpattern[] = {"varname",".","arrname","[","","]",NULL};
    badpattern[0] = varname;
    badpattern[2] = arrname;
    const TOKEN *tok2 = findtoken( tok1, badpattern );
    while (tok2)
    {
        if ( IsNumber( getstr(tok2, 4) ) )
        {
            if ( atoi( getstr(tok2, 4) ) >= arrsize )
            {
                std::ostringstream errmsg;
                errmsg << FileLine(tok2) << ": Array index out of bounds";
                ReportErr(errmsg.str());
            }
        }
        tok2 = findtoken( tok2->next, badpattern );
    }
}
//---------------------------------------------------------------------------

static void CheckBufferOverrun_StructVariable()
{
    const char *declstruct_pattern[] = {"struct","","{",0};
    for ( const TOKEN * tok = findtoken( tokens, declstruct_pattern );
          tok;
          tok = findtoken( tok->next, declstruct_pattern ) )
    {
        const char *structname = tok->next->str;

        if ( ! IsName( structname ) )
            continue;

        // Found a struct declaration. Search for arrays..
        for ( TOKEN * tok2 = tok->next->next; tok2; tok2 = tok2->next )
        {
            if ( tok2->str[0] == '}' )
                break;

            if ( strchr( ";{,(", tok2->str[0] ) )
            {
                // Declare array..
                if ( match(tok2->next, "var var [ num ] ;") )
                {
                    const char *arrname = getstr(tok2, 2);
                    const char *arrsize = getstr(tok2, 4);

                    for ( const TOKEN *tok3 = tokens; tok3; tok3 = tok3->next )
                    {
                        if ( strcmp(tok3->str, structname) )
                            continue;

                        // Declare variable: Fred fred1;
                        if ( match( tok3->next, "var ;" ) )
                        {
                            const char *varname = tok3->next->str;
                            CheckBufferOverrun_StructVariable_CheckVar( tok3, varname, arrname, atoi(arrsize) );
                        }

                        // Declare pointer: Fred *fred1
                        else if ( match(tok3->next, "* var") && tok3->next->next->next && strchr(",);=", tok3->next->next->next->str[0]) )
                        {
                            const char *varname = tok3->next->next->str;
                            CheckBufferOverrun_StructVariable_CheckVar( tok3, varname, arrname, atoi(arrsize) );
                        }
                    }
                }
            }
        }
    }
}
//---------------------------------------------------------------------------

void CheckBufferOverrun()
{
    CheckBufferOverrun_LocalVariable();
    CheckBufferOverrun_StructVariable();
}
//---------------------------------------------------------------------------








//---------------------------------------------------------------------------
// Dangerous functions
//---------------------------------------------------------------------------

void WarningDangerousFunctions()
{
    for (const TOKEN *tok = tokens; tok; tok = tok->next)
    {
        if (match(tok, "gets ("))
        {
            std::ostringstream ostr;
            ostr << FileLine(tok) << ": Found 'gets'. You should use 'fgets' instead";
            ReportErr(ostr.str());
        }

        else if (match(tok, "scanf (") && strcmp(getstr(tok,2),"\"%s\"") == 0)
        {
            std::ostringstream ostr;
            ostr << FileLine(tok) << ": Found 'scanf'. You should use 'fgets' instead";
            ReportErr(ostr.str());
        }
    }
}
//---------------------------------------------------------------------------




