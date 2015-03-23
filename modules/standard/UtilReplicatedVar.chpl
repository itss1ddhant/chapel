/*
 * Copyright 2004-2015 Cray Inc.
 * Other additional copyright holders may be indicated within.
 * 
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 * 
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
Support for user-level replicated variables.

A "replicated" variable is a variable for which there is a copy on each locale.
Referencing a replicated variable (in a stylized way, see below)
accesses its copy on the current locale.

Features:

* The variable's copies are not synchronized automatically among the locales.

Limitations:

* It is "user-level", i.e. the user is required to handle the variable
  in specific ways to achieve the desired result.

* Tree-shape communication (like for reductions) is not provided.

* Using a replicated variable of an array type is not straightforward.
  Workaround: declare that array itself as replicated, then access it normally,
  e.g.::

   var replArray: [MyDomain dmapped ReplicatedDist()] real;

How to use replicated variables::

    use UtilReplicatedVar;

    // declare a replicated variable of the type 'MyType'
    var myRepVar: [rcDomain] MyType;

    // access its copy on the current locale (read or write) (either option)
    ... myRepVar(1) ...
    ... rcLocal(myRepVar) ...

    // "replicate": assign 'valToRep' to copies on all locales
    rcReplicate(myRepVar, valToRep);

    // "collect": assign from each copy of 'myRepVar' to
    // corresponding element of an array 'collected'
    var collected: [LocaleSpace] MyType;
    rcCollect(myRepVar, collected);

    // access directly a remote copy on the locale 'remoteLoc' (read or write)
    ... rcRemote(myRepVar, remoteLoc) ...

For more examples, see :proc:`rcExample` and :proc:`rcExampleOverLocales`
in the module source code.

----------------------------------------------
Advanced: replicating over a subset of locales
----------------------------------------------

To replicate a variable over a subset of locales, say ``myLocales``,
modify the above variable declarations as follows::

    var myRepVar: [rcDomainBase dmapped ReplicatedDist(myLocales,
                     "over which to replicate 'myRepVar'")] MyType;
    var collected: [myLocales.domain] MyType;

``myLocales`` must be "consistent", as defined for ReplicatedDist.
That is, for each ``ix`` in ``myLocales.domain``,
``myLocales[ix]`` must equal ``Locales[ix]``.

Tip: if the domain of the desired array of locales cannot be described
as a rectangular domain (which could be strided, multi-dimensional,
and/or sparse), make that array's domain associative over int.
*/
module UtilReplicatedVar {

use ReplicatedDist;

pragma "no doc"
const rcDomainIx   = 1;
/* Use this domain when replicating over a subset of locales,
   as shown above. */
const rcDomainBase = {rcDomainIx..rcDomainIx};
pragma "no doc"
const rcLocales    = Locales;
const rcDomainMap  = new ReplicatedDist(rcLocales);
/* Use this domain to declare a user-level replicated variable,
   as shown above. */
const rcDomain     = rcDomainBase dmapped new dmap(rcDomainMap);
pragma "no doc" // todo - remove this?  our examples use LocaleSpace instead
const rcCollectDomaim = rcLocales.domain;
pragma "no doc"
param _rcErr1 = " must be 'rcDomain' or 'rcDomainBase dmapped ReplicatedDist(an array of locales)'";

pragma "no doc"
proc _rcTargetLocalesHelper(replicatedVar: [?D])
  where replicatedVar._value.type: ReplicatedArr
{
  return replicatedVar._value.dom.dist.targetLocales;
}

pragma "no doc" // documented with the following entry
proc rcReplicate(replicatedVar: [?D] ?MYTYPE, valToReplicate: MYTYPE): void
{ compilerError("the domain of first argument to rcReplicate()", _rcErr1); }

/* Assign `valToReplicate` to copies of `replicatedVar` on all locales. */
proc rcReplicate(replicatedVar: [?D] ?MYTYPE, valToReplicate: MYTYPE): void
  where replicatedVar._value.type: ReplicatedArr
{
  assert(replicatedVar.domain == rcDomainBase);
  coforall loc in _rcTargetLocalesHelper(replicatedVar) do
    on loc do
      replicatedVar[rcDomainIx] = valToReplicate;
}

pragma "no doc" // documented with the following entry
proc rcCollect(replicatedVar: [?D] ?MYTYPE, collected: [?CD] MYTYPE): void
  where ! replicatedVar._value.type: ReplicatedArr
{ compilerError("the domain of first argument to rcCollect()", _rcErr1); }

/* Copy the value of `replicatedVar` on each locale
   into the element of `collected` that corresponds to that locale. */
proc rcCollect(replicatedVar: [?D] ?MYTYPE, collected: [?CD] MYTYPE): void
  where replicatedVar._value.type: ReplicatedArr
{
  var targetLocales = _rcTargetLocalesHelper(replicatedVar);
  assert(replicatedVar.domain == rcDomainBase);
  assert(collected.domain == targetLocales.domain);
  coforall (loc, col) in zip(targetLocales, collected) do
    on loc do
      col = replicatedVar[rcDomainIx];
}

/*
Access the copy of `replicatedVar` on the current locale.

This is equivalent to ``replicatedVar[1]``.
*/
proc rcLocal(replicatedVar: [?D] ?MYTYPE) ref: MYTYPE {
  return replicatedVar[rcDomainIx];
}

/*
Access the copy of `replicatedVar` on the locale `remoteLoc`.
*/
proc rcRemote(replicatedVar: [?D] ?MYTYPE, remoteLoc: locale) ref: MYTYPE {
  var result: MYTYPE;
  on remoteLoc do
    result = replicatedVar[rcDomainIx];
  return result;
}


/////////////////////////////////////////////////////////////////////////////
// code examples

/*
This example illustrates the use of a replicated variable as follows:

* creates a replicated variable of the type `MyType`,
* initializes all its copies to `initVal`,
* stores `newVal` into its copy on the locale `newLocale`,
* collects all its copies into an array `collected`.

See $CHPL_HOME/modules/standard/UtilReplicatedVar.chpl
for the source code.
*/
proc rcExample(initVal: ?MyType, newVal: MyType, newLocale: locale): void {
  writeln("starting rcExample");

  // declare a replicated variable
  var myRepVar: [rcDomain] MyType;

  // initialize all copies to 'initVal'
  rcReplicate(myRepVar, initVal);

  writeln("\nafter initialization, myRepVar copies are:\n", myRepVar);

  // go to 'newLocale' and update its copy to 'newVal'
  on newLocale {
    // myRepVar[1] references myRepVar's copy on the current locale
    writeln("\non ", newLocale, ", myRepVar is:\n",
            "  before assignment: ", myRepVar[1]);

    // update the current locale's copy
    myRepVar[1] = newVal;

    writeln("  after assignment:  ", myRepVar[1]);
  }

  writeln("\nafter update, myRepVar copies are:\n", myRepVar);

  // collect all copies of 'myRepVar' into an array
  var collected: [LocaleSpace] MyType;
  rcCollect(myRepVar, collected);

  writeln("\ncollected copies of myRepVar are:\n", collected);
}

/*
This is the same as :proc:`rcExample`, except the user can provide
specific locales to replicate over via the `localesToReplicateOver` argument.

See $CHPL_HOME/modules/standard/UtilReplicatedVar.chpl
for the source code.
*/
proc rcExampleOverLocales(initVal: ?MyType, newVal: MyType, newLocale: locale,
                          localesToReplicateOver: [] locale = Locales): void {
  writeln("starting rcExampleOverLocales over ", localesToReplicateOver);

  // declare a replicated variable
  // DIFFERENT from rcExample(): the domain in myRepVar's type
  var myRepVar: [rcDomainBase dmapped ReplicatedDist(localesToReplicateOver,
   "over which to replicate 'myRepVar' in rcExampleOverLocales()")] MyType;

  // initialize all copies to 'initVal'
  rcReplicate(myRepVar, initVal);

  writeln("\nafter initialization, myRepVar copies are:\n", myRepVar);

  // go to 'newLocale' and update its copy to 'newVal'
  on newLocale {
    // myRepVar[1] references myRepVar's copy on the current locale
    writeln("\non ", newLocale, ", myRepVar is:\n",
            "  before assignment: ", myRepVar[1]);

    // update the current locale's copy
    myRepVar[1] = newVal;

    writeln("  after assignment:  ", myRepVar[1]);
  }

  writeln("\nafter update, myRepVar copies are:\n", myRepVar);

  // collect all copies of 'myRepVar' into an array
  // DIFFERENT from rcExample(): the domain in collected's type
  var collected: [localesToReplicateOver.domain] MyType;
  rcCollect(myRepVar, collected);

  writeln("\ncollected copies of myRepVar are:\n", collected);
}

} // module UtilReplicatedVar
