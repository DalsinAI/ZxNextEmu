import sys,json
def bdec(b,i=0):
    c=b[i:i+1]
    if c==b'i':
        j=b.index(b'e',i); return int(b[i+1:j]), j+1
    if c==b'l':
        i+=1; r=[]
        while b[i:i+1]!=b'e':
            v,i=bdec(b,i); r.append(v)
        return r,i+1
    if c==b'd':
        i+=1; r={}
        while b[i:i+1]!=b'e':
            k,i=bdec(b,i); v,i=bdec(b,i); r[k]=v
        return r,i+1
    j=b.index(b':',i); n=int(b[i:j]); return b[j+1:j+1+n], j+1+n
t,_=bdec(open(sys.argv[1],'rb').read())
info=t[b'info']
out={}
for f in info[b'files']:
    p='/'.join(x.decode('utf-8','replace') for x in f[b'path'])
    out[p]=f[b'length']
json.dump(out,open(sys.argv[2],'w'))
print(info[b'name'], len(out))
