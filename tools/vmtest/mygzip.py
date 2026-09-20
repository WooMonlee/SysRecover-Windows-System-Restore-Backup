import gzip, sys
with open(sys.argv[1], 'rb') as f:
    data = f.read()
with gzip.open(sys.argv[2], 'wb') as g:
    g.write(data)
print('gzipped', sys.argv[2])
