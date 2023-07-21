import itertools, functools, operator, sys

def levenshtein_distance(s1, s2):
    m = len(s1)
    n = len(s2)

    distance = [[0] * (n + 1) for i in range(m + 1)]

    for i in range(1, m + 1):
        distance[i][0] = i
    for j in range(1, n + 1):
        distance[0][j] = j

    for j in range(1, n + 1):
        for i in range(1, m + 1):
            if s1[i - 1] == s2[j - 1]:
                substitution_cost = 0
            else:
                substitution_cost = 1

            distance[i][j] = min(distance[i - 1][j] + 1,
                                 distance[i][j - 1] + 1,
                                 distance[i - 1][j - 1] + substitution_cost)

    return distance[m][n]

mode = str(sys.argv[1])  # either "input" or "regex"
maxlen = int(sys.argv[2])  # Size of the input to generate
input_distance = levenshtein_distance(mode, "input")
regex_distance = levenshtein_distance(mode, "regex")
rprinted = 0
if input_distance < regex_distance:  # User typed input
    decorations = [['', 'b', 'c', 'd'], ['', 'c', 'b', 'd', 'e']]
    predictedprints = functools.reduce(operator.mul, map(len, decorations), 1)
    predictedprints *= (maxlen+1)
    print(predictedprints)

    rprinted = 0  # Number of regexes printed
    for j in range(maxlen+1):
        for first, last in itertools.product(*decorations):
            print(f"{first}{'a'*j}{last}")
            rprinted += 1
elif input_distance > regex_distance:
    def addlazy(symbols):
        return symbols + [symbol + '?' for symbol in symbols]

    symbols = addlazy(['*', '+', '?'])
    decorations = [['', 'b'], ['', 'c'], ['', '^'], ['', '$']]
    predictedprints = functools.reduce(operator.mul, map(len, decorations), 1)
    predictedprints *= (len(symbols)**(maxlen+1) - 1)//(len(symbols) - 1)
    print(predictedprints)

    for i in range(maxlen+1):
        for csymbol in itertools.product(symbols, repeat=i):
            par = 0  # Number of parenthesis
            symbol = list(csymbol)  # A mutable type
            for i in range(len(symbol)-1):
                if len(symbol[i]) == 1 and symbol[i+1][0] == '?':
                    symbol[i+1] = ')' + symbol[i+1]
                    par += 1
            
            for useb, usec, anchorb, anchore in itertools.product(*decorations):
                print(f"{anchorb}{useb}{'('*par}a{''.join(symbol)}{usec}{anchore}")
                rprinted += 1
else:  # Not clear
    exit(0)

assert(rprinted == predictedprints)
