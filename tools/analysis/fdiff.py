import pickle,difflib,sys
D=pickle.load(open('dante-gen.pkl','rb'));T=pickle.load(open('tron-gen.pkl','rb'))
a,b=int(sys.argv[1],16),int(sys.argv[2],16)
for l in difflib.unified_diff(D[a],T[b],'dante','tron',n=0,lineterm=''):
    if not l.startswith('@@'): print(l)
