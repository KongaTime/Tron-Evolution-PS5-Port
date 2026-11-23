import pickle,difflib,sys
D=pickle.load(open('dante-gen.pkl','rb'));T=pickle.load(open('tron-gen.pkl','rb'))
lo,hi=0x82BC0000,0x82C00000
TR=[(a,i) for a,i in sorted(T.items()) if lo<=a<hi]
print('tron d3d-range funcs',len(TR))
for a in sys.argv[1:]:
    a=int(a,16); ins=D[a]; res=[]
    for ta,ti in TR:
        r=difflib.SequenceMatcher(None,ins,ti,autojunk=False).ratio()
        res.append((r,ta,len(ti)))
    res.sort(reverse=True)
    print('%08X n=%d:'%(a,len(ins)),' '.join('%08X:%.2f/%d'%(t,r,n) for r,t,n in res[:4]))
