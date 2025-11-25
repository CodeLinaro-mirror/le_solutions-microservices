# VectorStoreSearchResultsPage


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The object type, which is always &#x60;vector_store.search_results.page&#x60; | 
**search_query** | **List[str]** |  | 
**data** | [**List[VectorStoreSearchResultItem]**](VectorStoreSearchResultItem.md) | The list of search result items. | 
**has_more** | **bool** | Indicates if there are more results to fetch. | 
**next_page** | **str** | The token for the next page, if any. | 

## Example

```python
from openapi_client.models.vector_store_search_results_page import VectorStoreSearchResultsPage

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreSearchResultsPage from a JSON string
vector_store_search_results_page_instance = VectorStoreSearchResultsPage.from_json(json)
# print the JSON string representation of the object
print(VectorStoreSearchResultsPage.to_json())

# convert the object into a dict
vector_store_search_results_page_dict = vector_store_search_results_page_instance.to_dict()
# create an instance of VectorStoreSearchResultsPage from a dict
vector_store_search_results_page_from_dict = VectorStoreSearchResultsPage.from_dict(vector_store_search_results_page_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


