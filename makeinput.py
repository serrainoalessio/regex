import re, itertools, functools, operator

def enrich(symbols):
    return symbols + [symbol + '?' for symbol in symbols]

symbols = enrich(['*', '+', '?'])
maxlen = 4

rprinted = 0  # Number of regexes printed
decorations = [['', 'b'], ['', 'c'], ['', '^'], ['', '$']]
predictedprints = functools.reduce(operator.mul, map(len, decorations), 1)
predictedprints *= (len(symbols)**(maxlen+1) - 1)//(len(symbols) - 1)
print(predictedprints)

for i in range(maxlen+1):
    for csymbol in itertools.product(symbols, repeat=i):
        par = 0
        symbol = list(csymbol)
        for i in range(len(symbol)-1):
            if len(symbol[i]) == 1 and symbol[i+1][0] == '?':
                symbol[i+1] = ')' + symbol[i+1]
                par += 1
        
        for useb, usec, anchorb, anchore in itertools.product(*decorations):
            restring = [anchorb, useb, '('*par, 'a', ''.join(symbol), usec, anchore]
            restring = ''.join(map(str, restring))
            restring = re.sub(r"\?\?([=/'()!<>-])", r'?\\\\?\1', restring)
            print(restring)
            rprinted += 1

assert(rprinted == predictedprints)
