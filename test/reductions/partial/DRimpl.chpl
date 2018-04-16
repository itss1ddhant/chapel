use OwnedObject;
use utilities;

// + reduce (shape=DOM) ARR

proc pr(DIMS,ARR) throws {
  const OP = new Owned(new SumReduceScanOp(eltType=ARR.eltType));
  return ARR.domain.dist.dsiPartialReduce(OP.borrow(), DIMS, ARR);
}

proc DefaultDist.dsiPartialReduce(const reduceOp, const resDimSpec,
                                  const srcArr)
  throws
{
  partRedEnsureArray(srcArr);
  const ref srcDom  = srcArr.domain;
  const     srcDims = srcDom.dims();

  const (resDom, resDims) =
    partRedCheckAndCreateResultDimensions(this, resDimSpec, srcArr, srcDims);

  var resArr: [resDom] srcArr.eltType = reduceOp.identity;
  const resReduceOp = new (reduceOp.type)(eltType=resArr.type);

  forall (srcIdx, srcElm) in zip(srcDom, srcArr)
    with (resReduceOp reduce resArr)
  {
    const resIdx = fullIdxToReducedIdx(resDims, srcDims, srcIdx);
    reduceOp.accumulateOntoState(resArr[resIdx], srcElm);
  }

  return  resArr;
}
